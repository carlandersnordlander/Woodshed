#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AudioFile.h"
#include "NoteSynth.h"
#include "ParametricEq.h"
#include "signalsmith-stretch.h"

namespace nam_ui
{

/// How far the speed control goes. A quarter speed is slow enough to pick apart anything; past
/// double, a phase vocoder stops sounding like music.
constexpr float kMinPlaybackSpeed = 0.25f;
constexpr float kMaxPlaybackSpeed = 2.0f;

/// More lanes than any stem separation produces, and more than fit on screen at a readable height.
constexpr size_t kMaxTracks = 16;

/// A named region of the timeline that plays over and over. Positions are in seconds, because
/// that is what survives a file being reloaded at a different rate.
struct Loop
{
  std::string name;
  double startSeconds = 0.0;
  double endSeconds = 0.0;

  bool Valid() const { return endSeconds > startSeconds; }
};

/// One track. Several of them play locked together from one position, which is what makes stems
/// work: they are not synchronised, they are read from the same clock.
struct Track
{
  int id = 0;
  std::shared_ptr<AudioFile> file;
  std::shared_ptr<PeakCache> peaks;
  /// What the lane is called. Starts as the file's name and is yours to change - "drums.wav" says
  /// what it came from, "Kit" says what it is.
  std::string name;
  float gainDb = 0.0f;
  bool muted = false;
  bool soloed = false;
  /// Five bands per track. Off by default, so a track costs nothing until you ask it to.
  bool eqEnabled = false;
  ParametricEqSettings eq;
};

/// \brief Plays tracks together, with a transport and loops.
///
/// The chain and the player share an output but nothing else: a backing track should not be run
/// through your amp, and your playing should not be affected by the track's level.
class Player
{
public:
  void Prepare(double sampleRate, unsigned int maxFrames);

  // --- speed and pitch ---
  //
  // One stretcher on the mix rather than one per track. Gain, mute and solo are all linear, so
  // mixing first gives the same result for a quarter of the work - and it keeps the stems
  // phase-coherent with each other, which stretching them separately would not.

  /// 1.0 is the recording's own speed. Pitch is unaffected.
  void SetSpeed(float speed) { mSpeed.store(std::clamp(speed, kMinPlaybackSpeed, kMaxPlaybackSpeed), std::memory_order_relaxed); }
  float GetSpeed() const { return mSpeed.load(std::memory_order_relaxed); }

  /// Transpose, in semitones, without changing the speed. Free, given the stretcher is there.
  void SetSemitones(float semitones) { mSemitones.store(std::clamp(semitones, -12.0f, 12.0f), std::memory_order_relaxed); }
  float GetSemitones() const { return mSemitones.load(std::memory_order_relaxed); }

  // --- tracks. UI thread; the audio thread only ever sees a snapshot. ---

  /// \return the new track's id
  int AddTrack(std::shared_ptr<AudioFile> file, std::shared_ptr<PeakCache> peaks);
  void RemoveTrack(int id);
  /// Moves a track to another place in the order. Everything that reads tracks - the mixer, the
  /// lanes, the filters - works from this one vector, so this is the whole of reordering.
  void MoveTrack(int id, size_t newIndex);
  void Clear();
  std::vector<Track> GetTracks() const;
  void SetTrack(const Track& track);

  /// The longest track, which is how far the timeline goes.
  double DurationSeconds() const;

  // --- transport ---

  void Play();
  void Pause();
  void TogglePlay();
  void Stop(); ///< pause and return to the start of the active loop, or to zero

  bool IsPlaying() const { return mPlaying.load(std::memory_order_relaxed); }
  double GetPositionSeconds() const;
  void SetPositionSeconds(double seconds);

  /// \brief Silences every track without touching any of their own mute and solo settings.
  ///
  /// What soloing the click means: the metronome is not one of these tracks - it is mixed in after
  /// the player - so "only the click" cannot be expressed by soloing something here. Kept apart
  /// from the per-track flags so that letting go of it puts the mix back exactly as it was.
  void SetSilenced(bool silenced) { mSilenced.store(silenced, std::memory_order_relaxed); }
  bool IsSilenced() const { return mSilenced.load(std::memory_order_relaxed); }

  void SetGainDb(float db) { mGainDb.store(db, std::memory_order_relaxed); }
  float GetGainDb() const { return mGainDb.load(std::memory_order_relaxed); }

  // --- loops ---

  std::vector<Loop> GetLoops() const;
  void SetLoops(std::vector<Loop> loops);
  /// Which loop is repeating, or -1 for none. Playback runs straight through when nothing is set.
  void SetActiveLoop(int index);
  int GetActiveLoop() const { return mActiveLoop.load(std::memory_order_relaxed); }

  /// Adds the player's output into an interleaved stereo buffer. Audio thread; allocates nothing.
  void Process(float* output, unsigned int frames);

  /// The transcription, played back. Mixed in with the tracks rather than after them, so it goes
  /// through the same loops, the same stretch and the same transposition they do.
  NoteSynth& GetSynth() { return mSynth; }

private:
  double mSampleRate = 48000.0;

  mutable std::mutex mMutex;
  std::vector<Track> mTracks;
  std::vector<Loop> mLoops;
  int mNextTrackId = 1;

  /// The audio thread's copy, refilled under the lock each block; capacity reserved up front so
  /// taking it never allocates.
  std::vector<Track> mSnapshot;

  /// One filter per slot, audio-thread only. A track's settings travel in the snapshot but its
  /// delay lines cannot: they belong to the stream, not to the copy taken this block.
  struct TrackFilter
  {
    int trackId = 0;
    StereoParametricEq eq;
  };
  std::array<TrackFilter, kMaxTracks> mTrackFilters{};

  NoteSynth mSynth;

  std::atomic<bool> mPlaying{false};
  /// Position in frames. Written by the audio thread, read by everyone.
  std::atomic<long long> mPosition{0};
  std::atomic<int> mActiveLoop{-1};
  std::atomic<float> mGainDb{0.0f};
  std::atomic<bool> mSilenced{false};
  std::atomic<float> mSpeed{1.0f};
  std::atomic<float> mSemitones{0.0f};

  // Audio-thread only. Sized in Prepare so nothing here allocates while playing.
  signalsmith::stretch::SignalsmithStretch<float> mStretch;
  std::vector<float> mMixLeft;
  std::vector<float> mMixRight;
  std::vector<float> mOutLeft;
  std::vector<float> mOutRight;
  float mAppliedSemitones = 0.0f;
  /// True while the stretcher is in the path, so leaving it can flush the tail once.
  bool mWasStretching = false;
};

} // namespace nam_ui
