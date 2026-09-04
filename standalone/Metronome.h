#pragma once

#include <atomic>

#include "NAM/dsp.h" // NAM_SAMPLE

namespace nam_ui
{

/// The character of a click. All three are synthesised rather than loaded from files: a metronome
/// that needs sample files is a metronome that can fail to find them.
enum class ClickVoice
{
  Beep, ///< a clean sine pip, the classic digital metronome
  Wood, ///< a short noisy transient over a low body, closer to a woodblock
  Tick ///< very short and high, for when the click should sit out of the way
};

/// One of the three roles a click can have. Each has its own voice and level, because the whole
/// point of an accent is that it does not sound like the beats around it.
struct ClickSound
{
  bool enabled = true;
  ClickVoice voice = ClickVoice::Beep;
  float levelDb = 0.0f;
};

struct MetronomeSettings
{
  bool running = false;
  float bpm = 120.0f;
  /// Beats in a bar, and what note value a beat is. BPM always counts the beat unit, so 6/8 at
  /// 120 gives 120 eighths a minute - unambiguous, if not always what a score means by it.
  int beatsPerBar = 4;
  int beatUnit = 4;
  /// 1 = off, 2 = eighths, 3 = triplets, 4 = sixteenths.
  int subdivision = 1;

  ClickSound accent; ///< the first beat of the bar
  ClickSound beat; ///< the others
  ClickSound sub; ///< between the beats

  float levelDb = -6.0f; ///< the whole metronome, against everything else

  MetronomeSettings();
};

/// \brief Generates the clicks and keeps the count.
///
/// Timing is carried in samples rather than blocks: a click placed at the start of whatever buffer
/// it fell in would be up to a buffer late, which at 256 frames is five milliseconds of jitter and
/// audible as sloppiness.
class Metronome
{
public:
  void Prepare(double sampleRate);
  /// Audio thread. Safe to call every block; only rebuilds what changed.
  void SetSettings(const MetronomeSettings& settings);
  /// Adds the clicks for this block into `buffer`. Audio thread; allocates nothing.
  void Process(NAM_SAMPLE* buffer, unsigned int frames);
  /// Starts the next beat immediately, so starting always lands on a downbeat.
  void Restart();

  /// \brief Places the count somewhere other than the start of a bar.
  ///
  /// What lets the click join a song already in progress: given how far through a beat the music
  /// is and which beat of the bar that is, the metronome picks up there instead of counting from
  /// one. Without it you would have to press start on exactly the right beat.
  ///
  /// Safe to call from any thread; the audio thread picks it up on its next block.
  /// \param fractionIntoBeat 0 at the beat, approaching 1 just before the next
  void RequestSync(double fractionIntoBeat, int beatIndex);

  /// Where we are inside the current beat, 0 to 1. For the visuals.
  float GetPhase() const { return mPhase.load(std::memory_order_relaxed); }
  /// Which beat of the bar, counted from 0.
  int GetBeat() const { return mBeat.load(std::memory_order_relaxed); }
  /// How many bars since the count started.
  int GetBar() const { return mBar.load(std::memory_order_relaxed); }

private:
  /// Fires one click of the given role.
  void Trigger(const ClickSound& sound, bool isAccent, bool isSub);

  double mSampleRate = 48000.0;
  MetronomeSettings mSettings;

  /// Samples from one subdivision step to the next, and how far through we are.
  double mStepSamples = 0.0;
  double mStepPosition = 0.0;
  int mStepInBeat = 0;

  // One voice slot. Clicks are far shorter than the gap between them at any usable tempo, so a
  // retrigger cutting the previous one off is not something you can hear.
  double mOscPhase = 0.0;
  double mOscStep = 0.0;
  double mEnvelope = 0.0;
  double mEnvelopeDecay = 0.0;
  double mNoiseAmount = 0.0;
  float mVoiceGain = 0.0f;
  unsigned int mNoiseState = 22222u;

  /// Applies a pending RequestSync. Audio thread, at the top of a block.
  void ApplySync();

  std::atomic<bool> mSyncPending{false};
  std::atomic<double> mSyncFraction{0.0};
  std::atomic<int> mSyncBeat{0};

  std::atomic<float> mPhase{0.0f};
  std::atomic<int> mBeat{0};
  std::atomic<int> mBar{0};
};

} // namespace nam_ui
