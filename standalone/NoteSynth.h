#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace nam_ui
{

/// The voices on offer. Not a sampled instrument between them - each is a handful of harmonics and
/// an envelope, which is enough to tell a plucked string from a struck one and costs nothing.
enum class SynthInstrument
{
  Piano,
  ElectricPiano,
  Guitar,
  Bass,
  Organ,
  Sine,
  Count
};

const char* SynthInstrumentName(SynthInstrument instrument);

/// One note to play, in samples at the stream's rate - the same clock the player's position runs
/// on, so no conversion happens on the audio thread.
struct SynthNote
{
  long long startSample = 0;
  long long endSample = 0;
  int midi = 60;
  float velocity = 0.8f;
  int part = 0; ///< which SynthPart it belongs to
};

/// One track's worth of playback settings.
struct SynthPart
{
  SynthInstrument instrument = SynthInstrument::Piano;
  float gainDb = -6.0f;
};

/// \brief Plays a list of notes in time with the transport.
///
/// Rendered into the player's mix rather than beside it, which is what keeps it in step: the notes
/// go through the same loop wrapping, the same time stretch and the same transposition as the
/// audio they were found in, so slowing a song down slows the transcription with it and neither
/// can drift from the other.
///
/// A wavetable per instrument, built once. Summing ten harmonics per voice per sample would be
/// several million sines a second; reading one interpolated table is two multiplies.
class NoteSynth
{
public:
  void Prepare(double sampleRate);

  /// UI thread. Replaces everything the synth plays. `notes` must be sorted by startSample.
  void SetScore(std::vector<SynthNote> notes, std::vector<SynthPart> parts);
  /// True when there is anything at all to play, so the player can skip the whole thing.
  bool HasScore() const { return mHasScore.load(std::memory_order_relaxed); }

  /// How far to transpose, to follow the player's own pitch shift. Applied when a note starts.
  void SetTranspose(float semitones) { mTranspose.store(semitones, std::memory_order_relaxed); }

  /// Audio thread, once per block. False when the score is being rebuilt and this block should be
  /// left alone; voices already sounding carry on either way.
  bool BeginBlock();
  void EndBlock();

  /// Audio thread, once per sample, between BeginBlock and EndBlock. `position` is where the
  /// player is now, and may jump backwards at a loop.
  void RenderSample(long long position, float& left, float& right);

  /// Silences everything. For stopping, where a ringing piano note would otherwise hang on.
  void AllNotesOff();

  /// \brief Sounds a note or a chord straight away, with no regard to the transport.
  ///
  /// For hearing what was found by pressing it: a note in the timeline is a claim about the music,
  /// and the only way to judge a claim like that is to hear it against what it was found in.
  ///
  /// Held for a fixed time rather than until a position the transport would have to reach, so it
  /// works with everything stopped - which is when you are most likely to be inspecting.
  ///
  /// UI thread. Never waits: an audition the audio thread was too busy to collect is dropped,
  /// which is the right answer for something you can simply press again.
  void Audition(const int* midi, int count, SynthInstrument instrument, float gainDb, float seconds);

  /// True while any voice is still sounding, so the player knows to keep rendering while stopped.
  bool Sounding() const { return mSounding.load(std::memory_order_relaxed); }

  /// Audio thread. Advances and mixes whatever is sounding, without a score and without a clock.
  /// What the player calls when the transport is not running but something was auditioned.
  void RenderIdle(float& left, float& right);

  /// Audio thread, once per block, before rendering. Moves anything waiting in the audition slot
  /// into voices. Never waits.
  void CollectAuditions();

  /// \brief Says the playhead has been put somewhere, so work out afresh what should be sounding.
  ///
  /// Called for every seek and every start. Without it, playing from the middle of a held chord is
  /// silent until the next one begins: the position carries on contiguously from where it was, so
  /// nothing looks like a jump, and the notes already under the playhead are never started.
  void Invalidate() { mInvalidated.store(true, std::memory_order_relaxed); }

private:
  static constexpr size_t kTableSize = 2048;
  static constexpr size_t kVoiceCount = 24;
  static constexpr size_t kInstrumentCount = static_cast<size_t>(SynthInstrument::Count);

  struct Voice
  {
    bool active = false;
    double phase = 0.0;
    double step = 0.0;
    float level = 0.0f;
    float velocity = 0.0f;
    float sustain = 0.0f;
    float attackRate = 0.0f;
    float decayCoefficient = 0.0f;
    float releaseCoefficient = 0.0f;
    float gain = 1.0f;
    long long endSample = 0;
    /// Samples left before this voice lets go, for one that was auditioned rather than played from
    /// the score. Negative means it ends at `endSample` on the transport's clock instead.
    long long hold = -1;
    size_t table = 0;
    int stage = 0; ///< 0 attack, 1 decay and sustain, 2 release
  };

  /// \return which voice it took, so an auditioned note can be told to end on its own clock
  size_t Trigger(const SynthNote& note, const SynthPart& part);
  /// One sample of every sounding voice, with the envelopes advanced. `useTransport` decides
  /// whether a voice ends at its position or after its hold.
  float MixVoices(long long position, bool useTransport);
  /// Puts the cursor at the first note starting at or after `position`. After a seek or a loop.
  void Reseek(long long position);

  double mSampleRate = 48000.0;
  std::array<std::array<float, kTableSize>, kInstrumentCount> mTables{};

  mutable std::mutex mMutex;
  std::vector<SynthNote> mNotes;
  std::vector<SynthPart> mParts;
  std::atomic<bool> mHasScore{false};
  std::atomic<float> mTranspose{0.0f};
  std::atomic<bool> mInvalidated{true};

  /// Held for the length of one audio block by BeginBlock.
  std::unique_lock<std::mutex> mBlockLock;

  /// One audition waiting to be picked up. A chord is several notes at once, so there is room for
  /// a handful; a lock of its own so an audition never waits on a score being rebuilt.
  static constexpr size_t kMaxAudition = 8;
  mutable std::mutex mAuditionMutex;
  std::array<int, kMaxAudition> mAuditionMidi{};
  int mAuditionCount = 0;
  SynthInstrument mAuditionInstrument = SynthInstrument::Piano;
  float mAuditionGainDb = -6.0f;
  long long mAuditionHold = 0;
  std::atomic<bool> mAuditionWaiting{false};
  /// So the player can tell whether to keep rendering with the transport stopped.
  std::atomic<bool> mSounding{false};

  // Audio thread only.
  std::array<Voice, kVoiceCount> mVoices{};
  size_t mCursor = 0;
  long long mLastPosition = -1;
  /// Picked up from mInvalidated once per block rather than tested per sample.
  bool mForceReseek = true;
};

} // namespace nam_ui
