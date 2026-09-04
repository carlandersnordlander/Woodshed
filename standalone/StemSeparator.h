#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nam_ui
{

/// \brief Splits a track into stems by driving Demucs.
///
/// Demucs is a PyTorch model, so it is run as a separate program rather than loaded in. That
/// means a working `demucs` on the machine - `pip install demucs` - which is a real thing to ask
/// of a person, but far less than several hundred megabytes of runtime in the installer for a
/// feature not everyone wants.
///
/// The alternative, an ONNX export running in-process, replaces this class and nothing else: the
/// job handling, the progress and the loading of stems back in as tracks are the same either way.
class StemSeparator
{
public:
  ~StemSeparator();

  StemSeparator(const StemSeparator&) = delete;
  StemSeparator& operator=(const StemSeparator&) = delete;
  StemSeparator() = default;

  /// What to ask Demucs for. The defaults are what it does on its own.
  struct Options
  {
    /// What to run - "demucs", a full path to it, or anything taking the same arguments. Empty
    /// means try the usual ways of reaching it.
    std::string command;
    /// htdemucs is the default four-stem model; htdemucs_ft is slower and better; htdemucs_6s
    /// splits guitar and piano out as well.
    std::string model = "htdemucs";
    /// One of "vocals", "bass", "drums", "other" to get that stem and everything else as a pair,
    /// which is far quicker than a full split. Empty for all stems.
    std::string twoStems;
    /// "cpu" or "cuda". Empty lets Demucs pick, which is what you want unless it picks wrong.
    std::string device;
    /// Repeat passes at shifted offsets and average them. Better, and this many times slower.
    int shifts = 0;
  };

  void Start(std::filesystem::path input, std::filesystem::path outputRoot, Options options);

  bool IsRunning() const { return mRunning.load(std::memory_order_relaxed); }

  /// The last thing worth saying about the run, in words. Never raw tool output: Demucs draws a
  /// progress bar out of Unicode block characters, and the UI font has Latin in it and nothing
  /// else, so passing that through puts a row of empty boxes on the screen.
  std::string GetStatus() const;

  /// How far through the current pass, 0..1, or negative before anything has been parsed.
  float GetProgress() const { return mProgress.load(std::memory_order_relaxed); }
  /// Which pass is running and how many there are. A plain model is one pass; a fine-tuned one is
  /// four models in a row, and --shifts multiplies that again.
  int GetPass() const { return mPass.load(std::memory_order_relaxed); }
  int GetPassCount() const { return mPassCount.load(std::memory_order_relaxed); }
  /// Seconds of audio done and in total, for the run's own sense of scale. Zero when unknown.
  float GetDoneSeconds() const { return mDoneSeconds.load(std::memory_order_relaxed); }
  float GetTotalSeconds() const { return mTotalSeconds.load(std::memory_order_relaxed); }
  /// Non-empty when the last run failed, and worth showing.
  std::string GetError() const;

  /// Hands over the stems a finished run produced, once. False while it is still working or when
  /// there is nothing new.
  bool Consume(std::vector<std::filesystem::path>& stems);

protected:
  /// Reads one line of the tool's output and turns it into progress, or into nothing. Protected so
  /// a test can feed it the lines Demucs really prints without starting Demucs.
  void Observe(const std::string& line);

private:
  void Run(std::filesystem::path input, std::filesystem::path outputRoot, Options options);
  void SetStatus(std::string status);

  std::thread mWorker;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mDone{false};

  std::atomic<float> mProgress{-1.0f};
  std::atomic<int> mPass{0};
  std::atomic<int> mPassCount{1};
  std::atomic<float> mDoneSeconds{0.0f};
  std::atomic<float> mTotalSeconds{0.0f};
  /// The last percentage seen, for noticing when the bar starts over on the next model.
  float mLastPercent = 0.0f;

  mutable std::mutex mMutex;
  std::string mStatus;
  std::string mError;
  std::vector<std::filesystem::path> mStems;
};

} // namespace nam_ui
