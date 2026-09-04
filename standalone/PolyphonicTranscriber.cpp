#include "PolyphonicTranscriber.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
/// Deliberately small and defensive. It reports what went wrong in a form the app can turn into a
/// sentence, because "the tool exited with code 1" is not something anybody can act on - whereas
/// "basic-pitch is not installed for this Python" is.
///
/// predict() is called with keywords and falls back to a bare call, so a release that renames an
/// argument costs the thresholds rather than the whole feature.
constexpr const char* kScript = R"PY(import sys, csv

def fail(kind, detail):
    print("NAM-ERROR " + kind + " " + str(detail).replace("\n", " "))
    sys.exit(2)

try:
    from basic_pitch.inference import predict
except ImportError as e:
    fail("import", e)
except Exception as e:
    fail("runtime", e)

try:
    from basic_pitch import ICASSP_2022_MODEL_PATH as MODEL
except Exception:
    MODEL = None

audio, out = sys.argv[1], sys.argv[2]
onset, frame, minlen = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
minhz, maxhz = float(sys.argv[6]), float(sys.argv[7])

print("Loading the model. The first run also downloads it.")

kwargs = dict(onset_threshold=onset, frame_threshold=frame, minimum_note_length=minlen)
if minhz > 0:
    kwargs["minimum_frequency"] = minhz
if maxhz > 0:
    kwargs["maximum_frequency"] = maxhz

try:
    if MODEL is not None:
        result = predict(audio, MODEL, **kwargs)
    else:
        result = predict(audio, **kwargs)
except TypeError:
    # An argument this version does not know. The defaults are still worth having.
    try:
        result = predict(audio)
    except Exception as e:
        fail("predict", e)
except Exception as e:
    fail("predict", e)

events = result[2]
print("Writing " + str(len(events)) + " notes")

try:
    with open(out, "w", newline="") as handle:
        writer = csv.writer(handle)
        for event in events:
            # (start seconds, end seconds, midi pitch, amplitude, pitch bends)
            writer.writerow(["%.4f" % float(event[0]), "%.4f" % float(event[1]), int(event[2]),
                             "%.4f" % float(event[3])])
except Exception as e:
    fail("write", e)

print("NAM-DONE")
)PY";

/// TensorFlow announces itself at length on every run - which CPU instructions it will use, which
/// GPU it did not find. None of it is progress, and all of it would sit in the dialog where the
/// progress should be.
bool IsRuntimeNoise(const std::string& line)
{
  static const char* kNoise[] = {"tensorflow/core", "oneDNN", "TF-TRT", "cuda_", "external/local_",
                                 "tensorflow/compiler", "absl::", "WARNING:"};
  for (const char* fragment : kNoise)
    if (line.find(fragment) != std::string::npos)
      return true;
  return false;
}

/// Turns the script's own error line into something worth reading.
std::string DescribeFailure(const std::string& line)
{
  const std::string kind = line.substr(10, line.find(' ', 10) == std::string::npos ? std::string::npos
                                                                                  : line.find(' ', 10) - 10);
  const size_t detailAt = line.find(' ', 10);
  const std::string detail = (detailAt == std::string::npos) ? std::string() : line.substr(detailAt + 1);

  if (kind == "import")
    return "basic-pitch is not installed for this Python. Install it with \"pip install basic-pitch\". "
           "It needs a machine-learning runtime, which on a very new Python may not have a build yet - "
           "Python 3.11 or 3.12 is the safe ground. (" + detail + ")";
  if (kind == "predict")
    return "basic-pitch could not transcribe that track: " + detail;
  if (kind == "write")
    return "basic-pitch finished but its notes could not be written: " + detail;
  return "basic-pitch failed: " + detail;
}

} // namespace

PolyphonicTranscriber::~PolyphonicTranscriber()
{
  if (mWorker.joinable())
    mWorker.join();
}

void PolyphonicTranscriber::SetStatus(std::string status)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mStatus = std::move(status);
}

std::string PolyphonicTranscriber::GetStatus() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mStatus;
}

std::string PolyphonicTranscriber::GetError() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mError;
}

void PolyphonicTranscriber::Start(std::filesystem::path input, std::filesystem::path workFolder, Options options,
                                  int trackId)
{
  if (mRunning.load(std::memory_order_relaxed) || input.empty())
    return;

  if (mWorker.joinable())
    mWorker.join();

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError.clear();
    mResult = NoteTrack();
    mStatus = "Starting...";
  }

  mTrackId.store(trackId, std::memory_order_relaxed);
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

bool PolyphonicTranscriber::Consume(int& trackId, NoteTrack& out)
{
  if (!mDone.load(std::memory_order_acquire))
    return false;

  if (mWorker.joinable())
    mWorker.join();
  mDone.store(false, std::memory_order_relaxed);

  trackId = mTrackId.load(std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(mMutex);
  out = std::move(mResult);
  mResult = NoteTrack();
  return true;
}

bool PolyphonicTranscriber::WriteScript(const std::filesystem::path& path, std::string& error)
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

NoteTrack PolyphonicTranscriber::ReadNotes(const std::filesystem::path& path)
{
  NoteTrack track;
  std::ifstream in(path);
  if (!in.is_open())
    return track;

  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty())
      continue;

    std::istringstream fields(line);
    std::string start;
    std::string end;
    std::string midi;
    std::string amplitude;
    if (!std::getline(fields, start, ',') || !std::getline(fields, end, ',') || !std::getline(fields, midi, ','))
      continue;
    std::getline(fields, amplitude, ',');

    DetectedNote note;
    try
    {
      note.startSeconds = std::stod(start);
      note.endSeconds = std::stod(end);
      note.midi = std::stoi(midi);
      note.confidence = amplitude.empty() ? 0.7f : std::clamp(std::stof(amplitude), 0.0f, 1.0f);
    }
    catch (const std::exception&)
    {
      continue; // a line that is not a note is not a reason to lose the ones that are
    }

    if (note.endSeconds > note.startSeconds && note.midi > 0 && note.midi < 128)
      track.rawNotes.push_back(note);
  }

  // basic-pitch emits notes as it finds them, voice by voice rather than left to right. Everything
  // that draws or steps through them assumes time order.
  std::sort(track.rawNotes.begin(), track.rawNotes.end(),
            [](const DetectedNote& a, const DetectedNote& b) { return a.startSeconds < b.startSeconds; });

  // The model reports everything it can hear, harmonics and reverb tails included, and on a full
  // mix that is several times as many notes as were played. Cleaned here rather than by asking the
  // model for less, because what it reported is worth keeping: the setting can then be moved and
  // the result seen at once, instead of transcribing the track again to try another threshold.
  track.polyphonic = true;
  track.maxVoices = 6;
  // Measured on a real transcription: 1555 notes reported for three and a half minutes, of which
  // this keeps 965, and the average note goes from 300 ms to 480 ms because the ones that were one
  // note reported four times are one note again. Set here rather than lower because the complaint
  // about a transcription is always that there is too much of it.
  track.cleanupStrength = 0.65f;
  ApplyNoteCleanup(track);

  track.valid = true;
  return track;
}

void PolyphonicTranscriber::Run(std::filesystem::path input, std::filesystem::path workFolder, Options options)
{
  std::error_code ec;
  std::filesystem::create_directories(workFolder, ec);

  const std::filesystem::path script = workFolder / "transcribe.py";
  const std::filesystem::path notes = workFolder / "notes.csv";

  // A stale file from a previous run would otherwise be read as this run's answer.
  std::filesystem::remove(notes, ec);

  std::string error;
  if (!WriteScript(script, error))
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = error;
    return;
  }

  char numbers[6][32];
  std::snprintf(numbers[0], sizeof(numbers[0]), "%.4f", options.onsetThreshold);
  std::snprintf(numbers[1], sizeof(numbers[1]), "%.4f", options.frameThreshold);
  std::snprintf(numbers[2], sizeof(numbers[2]), "%.2f", options.minimumNoteMs);
  std::snprintf(numbers[3], sizeof(numbers[3]), "%.2f", options.minimumHz);
  std::snprintf(numbers[4], sizeof(numbers[4]), "%.2f", options.maximumHz);

  std::wstring arguments = L" " + QuoteArgument(script.wstring());
  arguments += L" " + QuoteArgument(input.wstring());
  arguments += L" " + QuoteArgument(notes.wstring());
  for (int i = 0; i < 5; i++)
    arguments += L" " + ToWide(numbers[i]);

  std::vector<std::string> candidates;
  if (!options.command.empty())
    candidates.push_back(options.command);
  else
    candidates = {"python", "py", "python3"};

  // The script says what went wrong in its own words; the last ordinary line is the progress.
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
               ? "Could not find Python. Transcribing chords needs Python with basic-pitch installed "
                 "(\"pip install basic-pitch\"); if Python is not on PATH, set the full path to it in "
                 "Settings."
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
    mError = "basic-pitch exited with code " + std::to_string(exitCode) + ". " + mStatus;
    return;
  }

  NoteTrack found = ReadNotes(notes);

  std::lock_guard<std::mutex> lock(mMutex);
  if (found.rawNotes.empty())
    mError = "basic-pitch finished but found no notes in that track.";
  else
    mStatus = "Transcribed " + std::to_string(found.rawNotes.size()) + " notes, kept "
              + std::to_string(found.notes.size()) + ".";
  mResult = std::move(found);
}

} // namespace nam_ui
