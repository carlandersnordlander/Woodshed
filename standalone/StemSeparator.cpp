#include "StemSeparator.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "Subprocess.h"

namespace nam_ui
{

void StemSeparator::Observe(const std::string& line)
{
  // Demucs writes a tqdm bar: "  6%|####### | 23.4/397.8 [00:06<01:40, 3.73seconds/s]". The bar
  // itself is Unicode block characters, so nothing here is ever shown as it arrives - the numbers
  // are read out of it and the drawing is the app's own.
  const size_t percentAt = line.find('%');
  if (percentAt != std::string::npos && percentAt > 0)
  {
    size_t start = percentAt;
    while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == ' '))
      start--;

    try
    {
      const std::string digits = line.substr(start, percentAt - start);
      if (!digits.empty() && digits.find_first_of("0123456789") != std::string::npos)
      {
        const float percent = std::stof(digits);
        if (percent >= 0.0f && percent <= 100.0f)
        {
          // A bar that starts over means the next model in a bag has begun. Counting them is the
          // only way to know which pass this is: the tool does not say.
          if (percent + 20.0f < mLastPercent)
            mPass.fetch_add(1, std::memory_order_relaxed);
          mLastPercent = percent;

          if (mPass.load(std::memory_order_relaxed) == 0)
            mPass.store(1, std::memory_order_relaxed);
          mProgress.store(percent / 100.0f, std::memory_order_relaxed);
        }
      }
    }
    catch (const std::exception&)
    {
      // Not a number after all. The bar is decoration; losing one frame of it costs nothing.
    }

    // "23.4/397.8" - how much audio is through and how much there is.
    const size_t slash = line.find('/', percentAt);
    if (slash != std::string::npos)
    {
      try
      {
        size_t from = slash;
        while (from > 0 && (std::isdigit(static_cast<unsigned char>(line[from - 1])) || line[from - 1] == '.'))
          from--;
        size_t to = slash + 1;
        while (to < line.size() && (std::isdigit(static_cast<unsigned char>(line[to])) || line[to] == '.'))
          to++;

        const float done = std::stof(line.substr(from, slash - from));
        const float total = std::stof(line.substr(slash + 1, to - slash - 1));
        if (total > 0.0f && done >= 0.0f && done <= total * 1.05f)
        {
          mDoneSeconds.store(done, std::memory_order_relaxed);
          mTotalSeconds.store(total, std::memory_order_relaxed);
        }
      }
      catch (const std::exception&)
      {
      }
    }
    return; // a bar line has nothing to say in words
  }

  // Anything that is not a bar is a sentence the tool wanted to say. Keep the ones that mean
  // something and drop the rest.
  if (line.find("Separating track") != std::string::npos)
  {
    SetStatus("Reading the track");
    return;
  }
  if (line.find("Downloading") != std::string::npos || line.find("%|") != std::string::npos)
    return;
  if (line.size() > 3 && line.find("Warning") == std::string::npos)
    SetStatus(line);
}

StemSeparator::~StemSeparator()
{
  if (mWorker.joinable())
    mWorker.join();
}

void StemSeparator::SetStatus(std::string status)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mStatus = std::move(status);
}

std::string StemSeparator::GetStatus() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mStatus;
}

std::string StemSeparator::GetError() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mError;
}

void StemSeparator::Start(std::filesystem::path input, std::filesystem::path outputRoot, Options options)
{
  if (mRunning.load(std::memory_order_relaxed) || input.empty())
    return;

  if (mWorker.joinable())
    mWorker.join();

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError.clear();
    mStems.clear();
    mStatus = "Loading the model";
  }

  mProgress.store(-1.0f, std::memory_order_relaxed);
  mPass.store(0, std::memory_order_relaxed);
  mDoneSeconds.store(0.0f, std::memory_order_relaxed);
  mTotalSeconds.store(0.0f, std::memory_order_relaxed);
  mLastPercent = 0.0f;

  mRunning.store(true, std::memory_order_relaxed);
  mDone.store(false, std::memory_order_relaxed);

  mWorker = std::thread(
    [this, input, outputRoot, options]()
    {
      Run(input, outputRoot, options);
      mDone.store(true, std::memory_order_release);
      mRunning.store(false, std::memory_order_relaxed);
    });
}

bool StemSeparator::Consume(std::vector<std::filesystem::path>& stems)
{
  if (!mDone.load(std::memory_order_acquire))
    return false;

  if (mWorker.joinable())
    mWorker.join();
  mDone.store(false, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(mMutex);
  stems = mStems;
  return true;
}

void StemSeparator::Run(std::filesystem::path input, std::filesystem::path outputRoot, Options options)
{
  std::error_code ec;
  std::filesystem::create_directories(outputRoot, ec);

  const std::wstring modelName = options.model.empty() ? L"htdemucs" : ToWide(options.model);
  const std::string& command = options.command;

  std::wstring arguments = L" -n " + modelName;
  if (!options.twoStems.empty())
    arguments += L" --two-stems=" + ToWide(options.twoStems);
  if (!options.device.empty())
    arguments += L" -d " + ToWide(options.device);
  if (options.shifts > 0)
    arguments += L" --shifts " + std::to_wstring(options.shifts);
  arguments += L" -o " + QuoteArgument(outputRoot.wstring());
  arguments += L" " + QuoteArgument(input.wstring());

  // What to try, in order. An explicit setting is taken at its word; otherwise the usual ways of
  // reaching a pip-installed program on Windows.
  std::vector<std::string> candidates;
  if (!command.empty())
    candidates.push_back(command);
  else
    candidates = {"demucs", "python -m demucs", "py -m demucs"};

  // How many times the bar will run: a fine-tuned model is four models one after another, and
  // shifts repeats the whole lot. Knowing it up front is what lets the dialog say "2 of 4".
  int passes = (options.model.find("_ft") != std::string::npos) ? 4 : 1;
  if (options.shifts > 0)
    passes *= options.shifts;
  mPassCount.store(passes, std::memory_order_relaxed);

  std::string startError;
  const int exitCode = RunProcess(candidates, arguments, [this](const std::string& line) { Observe(line); },
                                  startError);

  if (exitCode < 0)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = (startError == "not found")
               ? "Could not find Demucs. Install it with \"pip install demucs\", and if Python is not "
                 "on PATH either, set the full path to demucs.exe in Settings."
               : "Could not start Demucs (" + startError + ").";
    return;
  }

  if (exitCode != 0)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mError = "Demucs exited with code " + std::to_string(exitCode) + ". " + mStatus;
    return;
  }

  // Demucs writes to <out>/<model>/<track name>/{drums,bass,other,vocals}.wav. The track name is
  // the input's stem, but it sanitises it, so the folder is found rather than assumed.
  const std::filesystem::path modelFolder = outputRoot / (options.model.empty() ? "htdemucs" : options.model);
  std::vector<std::filesystem::path> found;

  if (std::filesystem::is_directory(modelFolder, ec))
  {
    std::filesystem::path best;
    std::filesystem::file_time_type newest{};
    for (const auto& entry : std::filesystem::directory_iterator(modelFolder, ec))
    {
      if (!entry.is_directory(ec))
        continue;
      const auto written = std::filesystem::last_write_time(entry.path(), ec);
      if (best.empty() || written > newest)
      {
        best = entry.path();
        newest = written;
      }
    }

    if (!best.empty())
    {
      for (const auto& entry : std::filesystem::directory_iterator(best, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".wav")
          found.push_back(entry.path());

      // Named order rather than whatever the filesystem hands back, so the tracks come out the
      // same way round every time.
      // Six-stem models add guitar and piano; a two-stem run names the second half "no_<stem>".
      static const char* kOrder[] = {"drums", "bass", "guitar", "piano", "other", "vocals"};
      std::sort(found.begin(), found.end(),
                [](const std::filesystem::path& a, const std::filesystem::path& b)
                {
                  const auto rank = [](const std::filesystem::path& p)
                  {
                    const std::string stem = p.stem().string();
                    for (int i = 0; i < 6; i++)
                      if (stem == kOrder[i])
                        return i;
                    return 6;
                  };
                  return rank(a) < rank(b);
                });
    }
  }

  std::lock_guard<std::mutex> lock(mMutex);
  if (found.empty())
    mError = "Demucs finished but no stems were found under " + modelFolder.string();
  else
    mStatus = "Separated into " + std::to_string(found.size()) + " stems.";
  mStems = std::move(found);
}

} // namespace nam_ui
