#include "Metronome.h"

#include <algorithm>
#include <cmath>

namespace nam_ui
{

namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// What each voice sounds like: the pitch of its body, how fast it dies away, and how much of the
/// attack is noise rather than tone.
struct VoiceShape
{
  double hz;
  double decaySeconds;
  double noise;
};

VoiceShape ShapeFor(ClickVoice voice)
{
  switch (voice)
  {
    case ClickVoice::Wood: return {620.0, 0.030, 0.55};
    case ClickVoice::Tick: return {2400.0, 0.012, 0.20};
    case ClickVoice::Beep:
    default: return {1000.0, 0.045, 0.0};
  }
}

double DbToGain(float db)
{
  return std::pow(10.0, static_cast<double>(db) / 20.0);
}
} // namespace

MetronomeSettings::MetronomeSettings()
{
  // The accent is the one you have to hear over what you are playing; the subdivision is the one
  // that has to stay out of the way.
  accent.voice = ClickVoice::Beep;
  accent.levelDb = 0.0f;
  beat.voice = ClickVoice::Beep;
  beat.levelDb = -4.0f;
  sub.voice = ClickVoice::Tick;
  sub.levelDb = -12.0f;
  sub.enabled = true;
}

void Metronome::Prepare(double sampleRate)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
  mEnvelope = 0.0;
  Restart();
}

void Metronome::Restart()
{
  // Fire on the very next sample, so pressing start puts you on beat one rather than partway
  // through whatever the counter happened to be doing.
  mStepPosition = 0.0;
  mStepInBeat = 0;
  mBeat.store(0, std::memory_order_relaxed);
  mBar.store(0, std::memory_order_relaxed);
  mPhase.store(0.0f, std::memory_order_relaxed);
}

void Metronome::RequestSync(double fractionIntoBeat, int beatIndex)
{
  mSyncFraction.store(std::clamp(fractionIntoBeat, 0.0, 0.999999), std::memory_order_relaxed);
  mSyncBeat.store(std::max(0, beatIndex), std::memory_order_relaxed);
  mSyncPending.store(true, std::memory_order_release);
}

void Metronome::ApplySync()
{
  if (!mSyncPending.exchange(false, std::memory_order_acquire))
    return;

  const int subdivision = std::max(1, mSettings.subdivision);
  const int beatsPerBar = std::max(1, mSettings.beatsPerBar);
  const double fraction = mSyncFraction.load(std::memory_order_relaxed);
  const int beatIndex = mSyncBeat.load(std::memory_order_relaxed) % beatsPerBar;

  // mStepInBeat is the step that will fire next, not the one just played - so a position partway
  // through step k has k+1 coming, and a position in the last step of a beat has the next beat's
  // downbeat coming.
  const double beatSamples = mStepSamples * static_cast<double>(subdivision);
  const int elapsedSteps = static_cast<int>(fraction * static_cast<double>(subdivision));

  int nextStep = elapsedSteps + 1;
  int nextBeat = beatIndex;
  if (nextStep >= subdivision)
  {
    nextStep = 0;
    nextBeat = (beatIndex + 1) % beatsPerBar;
  }

  mStepInBeat = nextStep;
  mBeat.store(nextBeat, std::memory_order_relaxed);
  mStepPosition =
    (static_cast<double>(elapsedSteps + 1) / static_cast<double>(subdivision) - fraction) * beatSamples;
}

void Metronome::SetSettings(const MetronomeSettings& settings)
{
  const bool wasRunning = mSettings.running;
  mSettings = settings;

  const double bpm = std::clamp(static_cast<double>(settings.bpm), 20.0, 300.0);
  const int subdivision = std::max(1, settings.subdivision);
  const double beatSamples = 60.0 / bpm * mSampleRate;
  mStepSamples = beatSamples / static_cast<double>(subdivision);

  if (settings.running && !wasRunning)
    Restart();
}

void Metronome::Trigger(const ClickSound& sound, bool isAccent, bool isSub)
{
  if (!sound.enabled)
  {
    mEnvelope = 0.0;
    return;
  }

  const VoiceShape shape = ShapeFor(sound.voice);

  // The role transposes the voice rather than choosing a different one: an accent that is the same
  // sound a fifth up reads as the same metronome emphasising a beat, while a different sound reads
  // as a second metronome.
  double hz = shape.hz;
  if (isAccent)
    hz *= 1.5;
  else if (isSub)
    hz *= 0.75;

  mOscStep = kTwoPi * hz / mSampleRate;
  mOscPhase = 0.0;
  mNoiseAmount = shape.noise;
  mEnvelope = 1.0;
  // Per-sample multiplier that reaches about -60 dB after the decay time.
  mEnvelopeDecay = std::exp(-1.0 / (shape.decaySeconds * mSampleRate) * 6.9);
  mVoiceGain = static_cast<float>(DbToGain(sound.levelDb) * DbToGain(mSettings.levelDb));
}

void Metronome::Process(NAM_SAMPLE* buffer, unsigned int frames)
{
  if (!mSettings.running || mStepSamples <= 0.0)
  {
    mEnvelope = 0.0;
    return;
  }

  // A sync waiting from the UI is applied before any of this block is counted, so the click lands
  // where the song is now rather than where it was when the request was made.
  ApplySync();

  const int subdivision = std::max(1, mSettings.subdivision);
  const int beatsPerBar = std::max(1, mSettings.beatsPerBar);

  for (unsigned int i = 0; i < frames; i++)
  {
    // A step is due when the counter reaches zero, which is checked per sample so a click lands on
    // the sample it belongs to rather than at the top of the block.
    if (mStepPosition <= 0.0)
    {
      const bool onBeat = (mStepInBeat == 0);
      if (onBeat)
      {
        const int beat = mBeat.load(std::memory_order_relaxed);
        Trigger(beat == 0 ? mSettings.accent : mSettings.beat, beat == 0, false);
      }
      else
      {
        Trigger(mSettings.sub, false, true);
      }

      mStepPosition += mStepSamples;
      mStepInBeat++;
      if (mStepInBeat >= subdivision)
      {
        mStepInBeat = 0;
        const int beat = (mBeat.load(std::memory_order_relaxed) + 1) % beatsPerBar;
        mBeat.store(beat, std::memory_order_relaxed);
        if (beat == 0)
          mBar.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (mEnvelope > 1.0e-4)
    {
      // Xorshift, so the noise in a wood click costs nothing and needs no library.
      mNoiseState ^= mNoiseState << 13;
      mNoiseState ^= mNoiseState >> 17;
      mNoiseState ^= mNoiseState << 5;
      const double noise = (static_cast<double>(mNoiseState) / 2147483648.0) - 1.0;

      const double tone = std::sin(mOscPhase);
      const double sample = tone * (1.0 - mNoiseAmount) + noise * mNoiseAmount * mEnvelope;

      buffer[i] += static_cast<NAM_SAMPLE>(sample * mEnvelope * mVoiceGain);

      mOscPhase += mOscStep;
      if (mOscPhase > kTwoPi)
        mOscPhase -= kTwoPi;
      mEnvelope *= mEnvelopeDecay;
    }

    mStepPosition -= 1.0;
  }

  // Where the visuals sit inside the current beat. Derived from the step counter so the pendulum
  // and the sound cannot drift apart.
  const double throughStep = 1.0 - std::clamp(mStepPosition / mStepSamples, 0.0, 1.0);
  const double phase = (static_cast<double>(mStepInBeat) + throughStep) / static_cast<double>(subdivision);
  mPhase.store(static_cast<float>(std::clamp(phase, 0.0, 1.0)), std::memory_order_relaxed);
}

} // namespace nam_ui
