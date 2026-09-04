#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "TempoAnalysis.h"

namespace nam_ui
{

/// \brief Finds the beats with Beat This!, the transformer model from ISMIR 2024.
///
/// The tracker in TempoAnalysis places beats within a few milliseconds of where they belong, and
/// on a plain four-to-the-floor it is right. What it gets wrong is which pulse is *the* beat: on a
/// slow song it counts the eighths, on a fast one the half notes. That is not a tuning problem -
/// choosing the metrical level is a judgement about how the music is heard, and the signal alone
/// does not settle it. Every attempt to score it from the audio here made some cases better and
/// others worse.
///
/// Beat This! was trained on that judgement, and it is where the whole gap between the classic
/// dynamic-programming trackers and the state of the art sits: roughly 0.6-0.8 F-measure against
/// roughly 0.9, almost entirely in metrical level and in music without drums.
///
/// It is a PyTorch model, so it runs as a separate process the same way Demucs and basic-pitch do,
/// and like them it is something the person installs before this does anything. The fast tracker
/// stays: it answers instantly and needs nothing, and this is the deeper pass for when it is
/// wrong.
class DeepBeatTracker
{
public:
  ~DeepBeatTracker();

  DeepBeatTracker(const DeepBeatTracker&) = delete;
  DeepBeatTracker& operator=(const DeepBeatTracker&) = delete;
  DeepBeatTracker() = default;

  struct Options
  {
    /// What to run. Empty means try the usual ways of reaching Python.
    std::string command;
    /// Post-process the model's output with a dynamic Bayesian network. Steadier on music that
    /// holds one tempo, and slower to follow one that does not - so it is off by default, which is
    /// also what the model's authors recommend.
    bool useDbn = false;
  };

  /// Starts on a worker thread. Does nothing while one is already running.
  /// \param workFolder somewhere to put the generated script and the beats it writes back
  void Start(std::filesystem::path input, std::filesystem::path workFolder, Options options);

  bool IsRunning() const { return mRunning.load(std::memory_order_relaxed); }

  /// The last line the tool printed. Loading the model takes most of the wait, and it says so.
  std::string GetStatus() const;
  /// Non-empty when the last run failed, and worth showing.
  std::string GetError() const;

  /// Hands over what a finished run produced, once.
  bool Consume(TempoEstimate& out);

private:
  void Run(std::filesystem::path input, std::filesystem::path workFolder, Options options);
  void SetStatus(std::string status);
  /// Writes the Python that does the work. Regenerated every run, so a new version of the app is
  /// never left driving an old script.
  static bool WriteScript(const std::filesystem::path& path, std::string& error);
  /// Turns the beats and downbeats the script wrote into the same estimate the fast tracker
  /// produces, so everything downstream cannot tell which one found them.
  static TempoEstimate ReadBeats(const std::filesystem::path& path);

  std::thread mWorker;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mDone{false};

  mutable std::mutex mMutex;
  std::string mStatus;
  std::string mError;
  TempoEstimate mResult;
};

} // namespace nam_ui
