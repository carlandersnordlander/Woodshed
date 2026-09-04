#include "NoteSynth.h"

#include <algorithm>
#include <cmath>

namespace nam_ui
{

namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// What an instrument is: the strength of its first ten harmonics, and how it starts and stops.
/// A struck string is a bright attack and a long fall to nothing; an organ is flat and holds.
struct InstrumentShape
{
  float harmonics[10];
  float attackSeconds;
  float decaySeconds;
  float sustain; ///< 0 for anything plucked or struck - it just falls away
  float releaseSeconds;
};

const InstrumentShape kShapes[static_cast<size_t>(SynthInstrument::Count)] = {
  // Piano: a full harmonic series falling away steadily.
  {{1.0f, 0.55f, 0.32f, 0.18f, 0.12f, 0.08f, 0.05f, 0.03f, 0.02f, 0.01f}, 0.004f, 2.2f, 0.0f, 0.25f},
  // Electric piano: almost a sine, with a strong fourth harmonic for the tine.
  {{1.0f, 0.12f, 0.06f, 0.40f, 0.04f, 0.03f, 0.02f, 0.01f, 0.0f, 0.0f}, 0.006f, 1.6f, 0.05f, 0.30f},
  // Guitar: brighter than a piano and shorter.
  {{1.0f, 0.62f, 0.48f, 0.31f, 0.24f, 0.16f, 0.11f, 0.07f, 0.05f, 0.03f}, 0.003f, 1.4f, 0.0f, 0.20f},
  // Bass: mostly fundamental, which is what makes it read as low rather than as muddy.
  {{1.0f, 0.40f, 0.20f, 0.10f, 0.06f, 0.03f, 0.02f, 0.0f, 0.0f, 0.0f}, 0.006f, 1.8f, 0.08f, 0.25f},
  // Organ: drawbars, and it holds for as long as the note does.
  {{1.0f, 0.80f, 0.55f, 0.0f, 0.40f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f}, 0.010f, 0.05f, 0.90f, 0.08f},
  // Sine: nothing but the pitch, for when the question is what the note is.
  {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 0.008f, 0.20f, 0.75f, 0.10f},
};

float DbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

/// Per-sample multiplier that falls to about -60 dB over `seconds`.
float DecayCoefficient(float seconds, double sampleRate)
{
  const double samples = std::max(1.0, static_cast<double>(seconds) * sampleRate);
  return static_cast<float>(std::exp(-6.9078 / samples));
}
} // namespace

const char* SynthInstrumentName(SynthInstrument instrument)
{
  switch (instrument)
  {
    case SynthInstrument::ElectricPiano: return "Electric piano";
    case SynthInstrument::Guitar: return "Guitar";
    case SynthInstrument::Bass: return "Bass";
    case SynthInstrument::Organ: return "Organ";
    case SynthInstrument::Sine: return "Sine";
    case SynthInstrument::Piano:
    default: return "Piano";
  }
}

void NoteSynth::Prepare(double sampleRate)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

  // One cycle of each instrument, summed from its harmonics and normalised. Built here so that
  // playing a note is a table read rather than ten sines.
  for (size_t i = 0; i < kInstrumentCount; i++)
  {
    const InstrumentShape& shape = kShapes[i];
    float peak = 0.0f;

    for (size_t s = 0; s < kTableSize; s++)
    {
      const double angle = kTwoPi * static_cast<double>(s) / static_cast<double>(kTableSize);
      double value = 0.0;
      for (size_t h = 0; h < 10; h++)
        if (shape.harmonics[h] != 0.0f)
          value += static_cast<double>(shape.harmonics[h]) * std::sin(angle * static_cast<double>(h + 1));

      mTables[i][s] = static_cast<float>(value);
      peak = std::max(peak, std::fabs(mTables[i][s]));
    }

    if (peak > 1.0e-6f)
      for (float& sample : mTables[i])
        sample /= peak;
  }

  AllNotesOff();
  mCursor = 0;
  mLastPosition = -1;
}

void NoteSynth::SetScore(std::vector<SynthNote> notes, std::vector<SynthPart> parts)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mNotes = std::move(notes);
  mParts = std::move(parts);
  mHasScore.store(!mNotes.empty(), std::memory_order_relaxed);
  mCursor = 0;
  mLastPosition = -1; // the cursor means nothing against the new list
}

bool NoteSynth::BeginBlock()
{
  if (!mHasScore.load(std::memory_order_relaxed))
    return false;

  // Never waits. A rebuild costs this block's new notes, not a dropout: whatever is already
  // sounding carries on, because the voices are not behind the lock.
  mBlockLock = std::unique_lock<std::mutex>(mMutex, std::try_to_lock);
  if (!mBlockLock.owns_lock())
    return false;

  // Read once here rather than tested at every sample.
  if (mInvalidated.exchange(false, std::memory_order_relaxed))
    mForceReseek = true;

  return true;
}

void NoteSynth::EndBlock()
{
  if (mBlockLock.owns_lock())
    mBlockLock.unlock();
}

void NoteSynth::Reseek(long long position)
{
  const SynthNote key{position, 0, 0, 0.0f, 0};
  const auto at = std::lower_bound(mNotes.begin(), mNotes.end(), key,
                                   [](const SynthNote& a, const SynthNote& b) { return a.startSample < b.startSample; });
  mCursor = static_cast<size_t>(std::distance(mNotes.begin(), at));

  // Notes that began before this point may well still be sounding, and landing in the middle of a
  // held chord should sound it rather than leave silence until the next one. They are behind the
  // cursor because the list is ordered by where notes start, not by where they end, so a bounded
  // walk backwards is what finds them - unbounded would mean rereading the whole song at every
  // loop wrap.
  constexpr size_t kLookback = 128;
  size_t scanned = 0;
  for (size_t i = mCursor; i-- > 0 && scanned < kLookback; scanned++)
  {
    const SynthNote& note = mNotes[i];
    if (note.endSample <= position)
      continue;
    if (note.part >= 0 && static_cast<size_t>(note.part) < mParts.size())
      Trigger(note, mParts[static_cast<size_t>(note.part)]);
  }
}

size_t NoteSynth::Trigger(const SynthNote& note, const SynthPart& part)
{
  // The quietest voice gives way. With two dozen of them this only happens on dense material, and
  // taking the quietest means what is stolen is the note nobody would have missed.
  size_t chosen = 0;
  float quietest = 1.0e9f;
  for (size_t i = 0; i < kVoiceCount; i++)
  {
    if (!mVoices[i].active)
    {
      chosen = i;
      quietest = -1.0f;
      break;
    }
    if (mVoices[i].level < quietest)
    {
      quietest = mVoices[i].level;
      chosen = i;
    }
  }

  const size_t table = static_cast<size_t>(part.instrument) % kInstrumentCount;
  const InstrumentShape& shape = kShapes[table];

  const double midi = static_cast<double>(note.midi) + static_cast<double>(mTranspose.load(std::memory_order_relaxed));
  const double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);

  Voice& voice = mVoices[chosen];
  voice.active = true;
  voice.phase = 0.0;
  voice.step = hz * static_cast<double>(kTableSize) / mSampleRate;
  voice.level = 0.0f;
  voice.velocity = std::clamp(note.velocity, 0.05f, 1.0f);
  voice.sustain = shape.sustain;
  voice.attackRate = 1.0f / std::max(1.0f, shape.attackSeconds * static_cast<float>(mSampleRate));
  voice.decayCoefficient = DecayCoefficient(shape.decaySeconds, mSampleRate);
  voice.releaseCoefficient = DecayCoefficient(shape.releaseSeconds, mSampleRate);
  voice.gain = DbToLinear(part.gainDb);
  voice.endSample = note.endSample;
  voice.hold = -1; // on the transport's clock unless the caller says otherwise
  voice.table = table;
  voice.stage = 0;
  return chosen;
}

void NoteSynth::Audition(const int* midi, int count, SynthInstrument instrument, float gainDb, float seconds)
{
  if (midi == nullptr || count <= 0)
    return;

  std::unique_lock<std::mutex> lock(mAuditionMutex, std::try_to_lock);
  if (!lock.owns_lock())
    return; // one still being collected; pressing again is the retry

  mAuditionCount = std::min(count, static_cast<int>(kMaxAudition));
  for (int i = 0; i < mAuditionCount; i++)
    mAuditionMidi[static_cast<size_t>(i)] = midi[i];

  mAuditionInstrument = instrument;
  mAuditionGainDb = gainDb;
  mAuditionHold = static_cast<long long>(static_cast<double>(std::max(0.05f, seconds)) * mSampleRate);
  mAuditionWaiting.store(true, std::memory_order_release);
}

void NoteSynth::CollectAuditions()
{
  if (!mAuditionWaiting.load(std::memory_order_acquire))
    return;

  std::unique_lock<std::mutex> lock(mAuditionMutex, std::try_to_lock);
  if (!lock.owns_lock())
    return; // next block will do

  SynthPart part;
  part.instrument = mAuditionInstrument;
  part.gainDb = mAuditionGainDb;

  for (int i = 0; i < mAuditionCount; i++)
  {
    SynthNote note;
    note.midi = mAuditionMidi[static_cast<size_t>(i)];
    note.velocity = 0.85f;
    note.startSample = 0;
    note.endSample = 0;

    // Trigger keys a voice to the transport; an audition is not on that clock, so the voice it
    // took counts down instead.
    mVoices[Trigger(note, part)].hold = mAuditionHold;
  }

  // Said here rather than left to the mixing to notice. The player only renders a stopped
  // transport when this is set, so waiting for MixVoices to set it meant it never would: nothing
  // rendered, so nothing said anything was sounding, so nothing rendered.
  if (mAuditionCount > 0)
    mSounding.store(true, std::memory_order_relaxed);

  mAuditionCount = 0;
  mAuditionWaiting.store(false, std::memory_order_release);
}

float NoteSynth::MixVoices(long long position, bool useTransport)
{
  float sum = 0.0f;
  bool anyActive = false;

  for (auto& voice : mVoices)
  {
    if (!voice.active)
      continue;

    if (voice.hold >= 0)
    {
      // Auditioned: it lets go after its own time, whatever the transport is or is not doing.
      if (voice.hold == 0 && voice.stage != 2)
        voice.stage = 2;
      if (voice.hold > 0)
        voice.hold--;
    }
    else if (useTransport && voice.stage != 2 && position >= voice.endSample)
    {
      voice.stage = 2;
    }

    switch (voice.stage)
    {
      case 0:
        voice.level += voice.attackRate;
        if (voice.level >= 1.0f)
        {
          voice.level = 1.0f;
          voice.stage = 1;
        }
        break;
      case 1:
        // Towards the sustain level, which for anything plucked is zero - so the same line both
        // holds an organ and lets a piano fall away.
        voice.level = voice.sustain + (voice.level - voice.sustain) * voice.decayCoefficient;
        break;
      default: voice.level *= voice.releaseCoefficient; break;
    }

    if (voice.level < 1.0e-4f && voice.stage == 2)
    {
      voice.active = false;
      voice.hold = -1;
      continue;
    }

    anyActive = true;

    const size_t index = static_cast<size_t>(voice.phase);
    const float fraction = static_cast<float>(voice.phase - static_cast<double>(index));
    const float a = mTables[voice.table][index & (kTableSize - 1)];
    const float b = mTables[voice.table][(index + 1) & (kTableSize - 1)];

    sum += (a + (b - a) * fraction) * voice.level * voice.velocity * voice.gain;

    voice.phase += voice.step;
    if (voice.phase >= static_cast<double>(kTableSize))
      voice.phase -= static_cast<double>(kTableSize);
  }

  mSounding.store(anyActive, std::memory_order_relaxed);
  return sum;
}

void NoteSynth::RenderSample(long long position, float& left, float& right)
{
  // A loop wrapping moves the playhead somewhere the cursor knows nothing about; so does a seek,
  // and so does pressing play, neither of which need look like a jump from here.
  if (mForceReseek || position != mLastPosition + 1)
  {
    Reseek(position);
    mForceReseek = false;
  }
  mLastPosition = position;

  while (mCursor < mNotes.size() && mNotes[mCursor].startSample <= position)
  {
    const SynthNote& note = mNotes[mCursor];
    if (note.endSample > position && note.part >= 0 && static_cast<size_t>(note.part) < mParts.size())
      Trigger(note, mParts[static_cast<size_t>(note.part)]);
    mCursor++;
  }

  const float sum = MixVoices(position, true);
  left += sum;
  right += sum;
}

void NoteSynth::RenderIdle(float& left, float& right)
{
  const float sum = MixVoices(0, false);
  left += sum;
  right += sum;
}

void NoteSynth::AllNotesOff()
{
  for (auto& voice : mVoices)
  {
    voice.active = false;
    voice.hold = -1;
  }
  mSounding.store(false, std::memory_order_relaxed);
}

} // namespace nam_ui
