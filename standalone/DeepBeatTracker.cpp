#include "DeepBeatTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "Subprocess.h"

namespace nam_ui
{

namespace
{

/// \brief The Python this drives.
///
/// Deliberately small and defensive, and it reports what went wrong in a form the app can turn
/// into a sentence: "beat_this is not installed for this Python" is something a person can act on,
/// and "the tool exited with code 1" is not.
///
/// File2Beats is the package's own one-call entry point - it opens the file, resamples it, runs
/// the model and returns two arrays of seconds - so this script does not have to know anything
/// about how the model is fed.
constexpr const char* kScript = R"PY(import sys

def fail(kind, detail):
    print("NAM-ERROR " + kind + " " + str(detail).replace("\n", " "))
    sys.exit(2)

try:
    from beat_this.inference import File2Beats
except ImportError as e:
    fail("import", e)
except Exception as e:
    fail("runtime", e)

audio, out = sys.argv[1], sys.argv[2]
use_dbn = sys.argv[3] == "1"

print("Loading the model. The first run also downloads it.")

try:
    import torch
    device = "cuda" if torch.cuda.is_available() else "cpu"
except Exception:
    device = "cpu"

print("Listening on the " + device)

try:
    tracker = File2Beats(checkpoint_path="final0", device=device, dbn=use_dbn)
except Exception as e:
    fail("load", e)

try:
    beats, downbeats = tracker(audio)
except Exception as e:
    fail("track", e)

beats = [float(t) for t in beats]
downbeats = set(round(float(t), 4) for t in downbeats)
print("Found " + str(len(beats)) + " beats")

try:
    with open(out, "w") as handle:
        for t in beats:
            # A beat, and whether it is the first of a bar. Downbeats come back as their own list
            # of times rather than as indices, so they are matched by time here where the two lists
            # are both in hand.
            handle.write("%.6f,%d\n" % (t, 1 if round(t, 4) in downbeats else 0))
except Exception as e:
    fail("write", e)

print("NAM-DONE")
)PY";

/// Torch and its friends announce themselves at length on every run. None of it is progress, and
/// all of it would sit in the panel where the progress should be.
bool IsRuntimeNoise(const std::string& line)
{
  static const char* kNoise[] = {"UserWarning", "FutureWarning", "warnings.warn", "torch/", "torchaudio",
                                 "site-packages", "WARNING:", "  "};
  for (const char* fragment : kNoise)
    if (line.find(fragment) != std::string::npos)
      return true;
  return false;
}

/// Turns the script's own error line into something worth reading.
std::string DescribeFailure(const std::string& line)
{
  const size_t detailAt = line.find(' ', 10);
  const std::string kind = line.substr(10, (detailAt == std::string::npos) ? std::string::npos : detailAt - 10);
  const std::string detail = (detailAt == std::string::npos) ? std::string() : line.substr(detailAt + 1);

  if (kind == "import")
    return "Beat This! is not installed for this Python. Install it with \"pip install "
           "https://github.com/CPJKU/beat_this/archive/main.zip\". It needs PyTorch, which on a very new "
           "Python may not have a build yet - 3.11 or 3.12 is the safe ground. ("
           + detail + ")";
  if (kind == "load")
    return "Beat This! could not load its model. The first run downloads it, so this is usually the "
           "network: " + detail;
  if (kind == "track")
    return "Beat This! could not follow that track: " + detail;
  if (kind == "write")
    return "Beat This! finished but its beats could not be written: " + detail;
  return "Beat This! failed: " + detail;
}

} // namespace

DeepBeatTracker::~DeepBeatTracker()
{
  if (mWorker.joinable())
    mWorker.join();
}

void DeepBeatTracker::SetStatus(std::string status)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mStatus = std::move(status);
}

std::string DeepBeatTracker::GetStatus() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mStatus;
}

std::string DeepBeatTracker::GetError() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mError;
}

void DeepBeatTracker::Start(std::filesystem::path input, std::filesystem::path workFolder, Options options)
{
  if (mRunning.load(std::memory_order_relaxed) || input.empty())
    return;

  if (mWorker.joinable())
    mWorker.join();

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError.clear();
    mResult = TempoEstimate();
    mStatus = "Starting...";
  }

  mRunning.store(true, std::memory_order_relaxed);
  mDone.store(false, std::memory_order_relaxed);

  mWorker = std::thread(
    [this, input, workFolder, options]()
    {
      Run(input, workFolder, options);
      mDone.store(true, std::memory_order_release);
      mRunning.store(false, std::memory_order_relaxed);
    });
}

bool DeepBeatTracker::Consume(TempoEstimate& out)
{
  if (!mDone.load(std::memory_order_acquire))
    return false;

  if (mWorker.joinable())
    mWorker.join();
  mDone.store(false, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(mMutex);
  out = std::move(mResult);
  mResult = TempoEstimate();
  return true;
}

bool DeepBeatTracker::WriteScript(const std::filesystem::path& path, std::string& error)
{
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open())
  {
    error = "Could not write " + path.string();
    return false;
  }
  out << kScript;
  if (!out.good())
  {
    error = "Could not finish writing " + path.string();
    return false;
  }
  return true;
}

TempoEstimate DeepBeatTracker::ReadBeats(const std::filesystem::path& path)
{
  TempoEstimate estimate;

  std::vector<double> beats;
  std::vector<bool> downbeat;

  std::ifstream in(path);
  if (!in.is_open())
    return estimate;

  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty())
      continue;

    std::istringstream fields(line);
    std::string at;
    std::string first;
    if (!std::getline(fields, at, ','))
      continue;
    std::getline(fields, first, ',');

    try
    {
      beats.push_back(std::stod(at));
      downbeat.push_back(!first.empty() && first[0] == '1');
    }
    catch (const std::exception&)
    {
      continue; // a line that is not a beat is not a reason to lose the ones that are
    }
  }

  if (beats.size() < 4)
    return estimate;

  estimate.beats = beats;
  estimate.downbeats.assign(downbeat.begin(), downbeat.end());
  estimate.firstBeatSeconds = beats.front();
  estimate.valid = true;

  // A dropped beat here and a stray downbeat there, mended against what the beats either side of
  // them say. Done before the tempo is measured, so the numbers below describe the repaired grid.
  RegulariseBeats(estimate);
  beats = estimate.beats;

  // The median interval, so a handful of doubled or dropped beats does not move the number the
  // user reads. The same rule the fast tracker uses, for the same reason.
  std::vector<double> intervals;
  intervals.reserve(beats.size());
  for (size_t i = 1; i < beats.size(); i++)
    intervals.push_back(beats[i] - beats[i - 1]);

  std::vector<double> sorted = intervals;
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
  const double median = sorted[sorted.size() / 2];
  estimate.bpm = (median > 0.0) ? static_cast<float>(60.0 / median) : 120.0f;

  // How far the tempo actually moved, over eight beats at a time so one loose snare does not read
  // as the song speeding up.
  constexpr size_t kSpan = 8;
  if (intervals.size() > kSpan)
  {
    double lowest = 1.0e9;
    double highest = 0.0;
    for (size_t i = 0; i + kSpan <= intervals.size(); i++)
    {
      double total = 0.0;
      for (size_t k = 0; k < kSpan; k++)
        total += intervals[i + k];
      const double local = 60.0 / (total / static_cast<double>(kSpan));
      lowest = std::min(lowest, local);
      highest = std::max(highest, local);
    }
    estimate.lowestBpm = static_cast<float>(lowest);
    estimate.highestBpm = static_cast<float>(highest);
  }

  // A model that was asked and answered. The fast tracker's confidence measures how far its winner
  // stood above the other candidates, which is a question this one is never asked.
  estimate.confidence = 1.0f;
  return estimate;
}

void DeepBeatTracker::Run(std::filesystem::path input, std::filesystem::path workFolder, Options options)
{
  std::error_code ec;
  std::filesystem::create_directories(workFolder, ec);

  const std::filesystem::path script = workFolder / "beats.py";
  const std::filesystem::path beats = workFolder / "beats.csv";

  // A stale file from a previous run would otherwise be read as this run's answer.
  std::filesystem::remove(beats, ec);

  std::string error;
  if (!WriteScript(script, error))
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = error;
    return;
  }

  std::wstring arguments = L" " + QuoteArgument(script.wstring());
  arguments += L" " + QuoteArgument(input.wstring());
  arguments += L" " + QuoteArgument(beats.wstring());
  arguments += options.useDbn ? L" 1" : L" 0";

  std::vector<std::string> candidates;
  if (!options.command.empty())
    candidates.push_back(options.command);
  else
    candidates = {"python", "py", "python3"};

  std::string scriptError;
  std::string startError;
  const int exitCode = RunProcess(candidates, arguments,
                                  [this, &scriptError](const std::string& line)
                                  {
                                    if (line.rfind("NAM-ERROR ", 0) == 0)
                                      scriptError = DescribeFailure(line);
                                    else if (line != "NAM-DONE" && !IsRuntimeNoise(line))
                                      SetStatus(line);
                                  },
                                  startError);

  if (exitCode < 0)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = (startError == "not found")
               ? "Could not find Python. The deep beat tracker needs Python with Beat This! installed; "
                 "if Python is not on PATH, set the full path to it in Settings."
               : "Could not start Python (" + startError + ").";
    return;
  }

  if (!scriptError.empty())
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = scriptError;
    return;
  }

  if (exitCode != 0)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = "Beat This! exited with code " + std::to_string(exitCode) + ". " + mStatus;
    return;
  }

  TempoEstimate found = ReadBeats(beats);

  std::lock_guard<std::mutex> lock(mMutex);
  if (!found.valid)
    mError = "Beat This! finished but found no beats in that track.";
  else
    mStatus = "Found " + std::to_string(found.beats.size()) + " beats at " + std::to_string(
                static_cast<int>(std::lround(found.bpm))) + " BPM.";
  mResult = std::move(found);
}

} // namespace nam_ui
