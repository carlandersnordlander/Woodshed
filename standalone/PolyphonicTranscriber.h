#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "NoteAnalysis.h"

namespace nam_ui
{

/// \brief Transcribes several notes at once, by driving Spotify's basic-pitch.
///
/// The detector in NoteAnalysis hears one note at a time and cannot be made to hear more: a chord
/// is periodic at the frequency its notes are all harmonics of, and no amount of tuning an
/// autocorrelator changes that. Chords need a model that was trained on them.
///
/// basic-pitch is Apache-licensed and small, but it is a Python program with a machine-learning
/// runtime under it, so it runs as a separate process the same way Demucs does - and, like Demucs,
/// it is something the person has to install before this does anything.
///
/// Driven through a generated script rather than through the `basic-pitch` command, because the
/// command's flags have changed between releases while `predict()` has not, and because it lets
/// the notes come back in a format worth parsing rather than as MIDI.
class PolyphonicTranscriber
{
public:
  ~PolyphonicTranscriber();

  PolyphonicTranscriber(const PolyphonicTranscriber&) = delete;
  PolyphonicTranscriber& operator=(const PolyphonicTranscriber&) = delete;
  PolyphonicTranscriber() = default;

  struct Options
  {
    /// What to run. Empty means try the usual ways of reaching Python.
    std::string command;
    /// How sure the model has to be that a note started. Higher finds fewer notes and invents
    /// fewer; lower catches quiet ones and also catches things nobody played.
    float onsetThreshold = 0.5f;
    /// The same question, asked about a note continuing rather than starting.
    float frameThreshold = 0.3f;
    /// Anything shorter than this is dropped, in milliseconds.
    float minimumNoteMs = 128.0f;
    /// The range to look in. Zero for either leaves it to the model - worth setting, because
    /// telling it there is no bass below your instrument's lowest string removes a whole class of
    /// wrong answers.
    float minimumHz = 0.0f;
    float maximumHz = 0.0f;
  };

  /// Starts on a worker thread. Does nothing while one is already running.
  /// \param workFolder somewhere to put the generated script and the notes it writes back
  void Start(std::filesystem::path input, std::filesystem::path workFolder, Options options, int trackId);

  bool IsRunning() const { return mRunning.load(std::memory_order_relaxed); }
  int RunningTrackId() const { return mRunning.load(std::memory_order_relaxed) ? mTrackId.load(std::memory_order_relaxed) : 0; }

  /// The last line the tool printed. Loading the model takes most of the wait, and it says so.
  std::string GetStatus() const;
  /// Non-empty when the last run failed, and worth showing.
  std::string GetError() const;

  /// Hands over what a finished run produced, once.
  bool Consume(int& trackId, NoteTrack& out);

private:
  void Run(std::filesystem::path input, std::filesystem::path workFolder, Options options);
  void SetStatus(std::string status);
  /// Writes the Python that does the work. Regenerated every run, so a new version of the app is
  /// never left driving an old script.
  static bool WriteScript(const std::filesystem::path& path, std::string& error);
  /// Reads the notes the script wrote back.
  static NoteTrack ReadNotes(const std::filesystem::path& path);

  std::thread mWorker;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mDone{false};
  std::atomic<int> mTrackId{0};

  mutable std::mutex mMutex;
  std::string mStatus;
  std::string mError;
  NoteTrack mResult;
};

} // namespace nam_ui
