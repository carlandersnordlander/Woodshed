#include "Tuner.h"

#include <algorithm>
#include <cmath>

namespace nam_ui
{

namespace
{
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 2.0 * kPi;

/// Time constant of the heterodyne smoother. Long enough that noise does not jitter the phase,
/// short enough that the display still reacts while you turn the peg.
constexpr double kSmoothingSeconds = 0.08;

/// Time constant of the cents reading. Much longer than the one above, because the cents figure is
/// a derivative of the phase and derivatives multiply whatever noise is on the signal.
constexpr double kCentsSmoothingSeconds = 0.22;

/// Readings further out than this are noise or a different string entirely, not a tuning error;
/// letting them into the smoother would drag it around for a quarter of a second each time.
constexpr double kMaxPlausibleCents = 200.0;

/// Below this the band is treated as silent; its phase would be meaningless noise.
constexpr double kSignalFloor = 1.0e-4;

/// Wrap to (-pi, pi].
double WrapPhase(double phase)
{
  while (phase > kPi)
    phase -= kTwoPi;
  while (phase <= -kPi)
    phase += kTwoPi;
  return phase;
}
} // namespace

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

double NoteFrequency(int noteIndex, int octave, double a4Hz)
{
  // Scientific pitch notation: C4 is MIDI 60, so A4 lands on 69 and returns a4Hz exactly.
  const int midi = (octave + 1) * 12 + noteIndex;
  return a4Hz * std::pow(2.0, (midi - 69) / 12.0);
}

std::string NoteName(int noteIndex, int octave)
{
  const int index = ((noteIndex % 12) + 12) % 12;
  return std::string(kNoteNames[index]) + std::to_string(octave);
}

void Tuner::Prepare(double sampleRate)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
  mSmoothing = 1.0 - std::exp(-1.0 / (mSampleRate * kSmoothingSeconds));
  mAppliedReferenceHz = 0.0; // forces the rotators to be rebuilt on the next block
  mHasPreviousPhase = false;
  mSmoothedCents = 0.0;
  mHasCents = false;

  // The detector runs at whatever integer division of the sample rate lands nearest its target,
  // so the same code covers 44.1k and 96k without a resampler.
  mDecimation = std::max(1, static_cast<int>(std::lround(mSampleRate / kDetectRateTarget)));
  mDetectRate = mSampleRate / static_cast<double>(mDecimation);
  mDecimateCount = 0;
  mDetectWrite = 0;
  mDetectFill = 0;
  mSinceDetect = 0;
  mAntiAliasA = 0.0;
  mAntiAliasB = 0.0;
  mDetect.fill(0.0f);
  mDetectedHz.store(0.0, std::memory_order_relaxed);

  // A one-pole at a quarter of the detector's Nyquist, twice over. Not a steep filter, but what
  // gets past it is a harmonic well above anything being tuned, and YIN is not troubled by those.
  const double cutoff = mDetectRate * 0.25;
  mAntiAliasCoeff = 1.0 - std::exp(-kTwoPi * cutoff / mSampleRate);

  for (auto& band : mBands)
  {
    band.accRe = 0.0;
    band.accIm = 0.0;
    band.oscRe = 1.0;
    band.oscIm = 0.0;
  }
}

void Tuner::SetReferenceHz(double hz)
{
  mReferenceHz.store(std::clamp(hz, 8.0, 5000.0), std::memory_order_relaxed);
}

float Tuner::GetBandPhase(size_t band) const
{
  return band < kStrobeBandCount ? mBands[band].phase.load(std::memory_order_relaxed) : 0.0f;
}

float Tuner::GetBandStrength(size_t band) const
{
  return band < kStrobeBandCount ? mBands[band].strength.load(std::memory_order_relaxed) : 0.0f;
}

/// \brief Estimates the pitch of what is in the decimated buffer, by YIN.
///
/// The difference function asks, for every lag, how far the signal is from itself that far back;
/// a periodic signal is nearly identical to itself one period later, so the answer dips at the
/// period. The cumulative mean normalisation is what makes it usable: raw differences fall off
/// towards long lags and would always favour the longest one on offer.
void Tuner::Detect()
{
  if (mDetectFill < kDetectWindow + kDetectMaxLag)
    return;

  // The newest window, oldest first. `at` walks the ring backwards from the write cursor.
  const size_t span = kDetectWindow + kDetectMaxLag;
  const size_t first = (mDetectWrite + kDetectSize - span) % kDetectSize;
  const auto at = [&](size_t i) { return static_cast<double>(mDetect[(first + i) % kDetectSize]); };

  // Too quiet to be a note. Without this the detector locks onto the noise floor between strings
  // and the display wanders while nothing is being played.
  double energy = 0.0;
  for (size_t i = 0; i < kDetectWindow; i++)
    energy += at(i) * at(i);
  if (energy / static_cast<double>(kDetectWindow) < 1.0e-6)
  {
    mDetectedHz.store(0.0, std::memory_order_relaxed);
    return;
  }

  mYin[0] = 1.0f;
  double runningSum = 0.0;
  size_t chosen = 0;
  bool below = false;

  // The first dip under the threshold wins, not the deepest. The deepest is usually an octave
  // down - twice a period is also a period - and taking it is the classic halving error.
  constexpr float kThreshold = 0.15f;
  constexpr size_t kShortestLag = 4;

  for (size_t lag = 1; lag <= kDetectMaxLag; lag++)
  {
    double difference = 0.0;
    for (size_t i = 0; i < kDetectWindow; i++)
    {
      const double delta = at(i) - at(i + lag);
      difference += delta * delta;
    }

    runningSum += difference;
    mYin[lag] = static_cast<float>(runningSum > 0.0 ? difference * static_cast<double>(lag) / runningSum : 1.0);

    if (lag < kShortestLag)
      continue;

    if (mYin[lag] < kThreshold)
    {
      // Inside a dip: keep the lowest point of it. Taking the first upturn instead reads the
      // period several samples short, because a dip this wide is not smooth - a bump on the way
      // down ends it early, and at low notes a few samples is a quarter of a semitone.
      if (!below || mYin[lag] < mYin[chosen])
        chosen = lag;
      below = true;
    }
    else if (below)
    {
      break; // out the far side; the bottom of that dip is the answer
    }
  }

  // Nothing periodic enough to name. Saying so is the right answer - a tuner that keeps showing
  // the last note it saw while you are not playing is worse than one that shows nothing.
  if (chosen == 0)
  {
    mDetectedHz.store(0.0, std::memory_order_relaxed);
    return;
  }

  // Parabolic interpolation through the three points around the minimum: the true period is
  // almost never a whole number of samples, and at these lags one sample is several cents.
  double period = static_cast<double>(chosen);
  if (chosen > 0 && chosen + 1 <= kDetectMaxLag)
  {
    const double before = mYin[chosen - 1];
    const double here = mYin[chosen];
    const double after = mYin[chosen + 1];
    const double divisor = 2.0 * (2.0 * here - before - after);
    if (std::fabs(divisor) > 1.0e-12)
      period += (after - before) / divisor;
  }

  const double hz = (period > 0.0) ? mDetectRate / period : 0.0;
  mDetectedHz.store((hz > 20.0 && hz < 2000.0) ? hz : 0.0, std::memory_order_relaxed);
}

void Tuner::Process(const float* input, unsigned int frames)
{
  if (input == nullptr || frames == 0)
    return;

  // --- feed the note detector ---
  //
  // Low-passed and decimated first: the estimate needs a couple of hundred milliseconds of signal,
  // and holding that at the full rate would be eight times the memory and eight times the work for
  // no more accuracy than a few kilohertz already gives.
  for (unsigned int i = 0; i < frames; i++)
  {
    const double x = static_cast<double>(input[i]);
    mAntiAliasA += mAntiAliasCoeff * (x - mAntiAliasA);
    mAntiAliasB += mAntiAliasCoeff * (mAntiAliasA - mAntiAliasB);

    if (++mDecimateCount < mDecimation)
      continue;
    mDecimateCount = 0;

    mDetect[mDetectWrite] = static_cast<float>(mAntiAliasB);
    mDetectWrite = (mDetectWrite + 1) % kDetectSize;
    mDetectFill = std::min(mDetectFill + 1, kDetectSize);
    mSinceDetect++;
  }

  // About twenty times a second. Often enough to follow a hand moving between strings, rarely
  // enough that the cost of it does not matter.
  if (mSinceDetect >= static_cast<size_t>(mDetectRate / 20.0))
  {
    mSinceDetect = 0;
    Detect();
  }

  const double reference = mReferenceHz.load(std::memory_order_relaxed);
  if (reference != mAppliedReferenceHz)
  {
    for (size_t b = 0; b < kStrobeBandCount; b++)
    {
      // Band b listens at reference * 2^b: the fundamental, its octave, and two octaves up.
      const double bandHz = reference * static_cast<double>(1 << b);
      const double step = kTwoPi * bandHz / mSampleRate;
      mBands[b].stepRe = std::cos(step);
      mBands[b].stepIm = -std::sin(step); // negative: shifts the band down to DC
      mBands[b].oscRe = 1.0;
      mBands[b].oscIm = 0.0;
      mBands[b].accRe = 0.0;
      mBands[b].accIm = 0.0;
    }
    mAppliedReferenceHz = reference;
    mHasPreviousPhase = false;
  }

  for (size_t b = 0; b < kStrobeBandCount; b++)
  {
    Band& band = mBands[b];
    double oscRe = band.oscRe;
    double oscIm = band.oscIm;
    double accRe = band.accRe;
    double accIm = band.accIm;

    for (unsigned int i = 0; i < frames; i++)
    {
      const double x = static_cast<double>(input[i]);

      // Multiply the input by the oscillator, then smooth: a one-pole low-pass around DC here is
      // a narrow band-pass around the band frequency in the original signal.
      accRe += mSmoothing * (x * oscRe - accRe);
      accIm += mSmoothing * (x * oscIm - accIm);

      // Advance the rotator by one sample: osc *= step.
      const double nextRe = oscRe * band.stepRe - oscIm * band.stepIm;
      oscIm = oscRe * band.stepIm + oscIm * band.stepRe;
      oscRe = nextRe;
    }

    // Rounding pulls the rotator off the unit circle over time; once a block is plenty.
    const double magnitude = std::sqrt(oscRe * oscRe + oscIm * oscIm);
    if (magnitude > 0.0)
    {
      oscRe /= magnitude;
      oscIm /= magnitude;
    }

    band.oscRe = oscRe;
    band.oscIm = oscIm;
    band.accRe = accRe;
    band.accIm = accIm;

    const double strength = std::sqrt(accRe * accRe + accIm * accIm);
    band.phase.store(static_cast<float>(std::atan2(accIm, accRe)), std::memory_order_relaxed);
    // Scaled so normal playing lands near 1; only used to fade bands that have nothing to show.
    band.strength.store(static_cast<float>(std::clamp(strength * 8.0, 0.0, 1.0)), std::memory_order_relaxed);
  }

  // Cents come from how fast the fundamental's phase turns: a full turn per second is one hertz
  // of error, which is a different number of cents depending on the note.
  const double fundamentalStrength = std::sqrt(mBands[0].accRe * mBands[0].accRe + mBands[0].accIm * mBands[0].accIm);
  const bool hasSignal = fundamentalStrength > kSignalFloor;
  mHasSignal.store(hasSignal, std::memory_order_relaxed);

  if (!hasSignal)
  {
    mHasPreviousPhase = false;
    mHasCents = false;
    return;
  }

  const double phase = std::atan2(mBands[0].accIm, mBands[0].accRe);
  if (mHasPreviousPhase)
  {
    const double blockSeconds = static_cast<double>(frames) / mSampleRate;
    const double deltaPhase = WrapPhase(phase - mPreviousPhase);
    const double deltaHz = deltaPhase / (kTwoPi * blockSeconds);
    const double played = reference + deltaHz;

    if (played > 0.0)
    {
      const double cents = 1200.0 * std::log2(played / reference);

      // Rejected before smoothing, not after. Clamping the average lets a single wild reading pull
      // it for as long as the time constant, which is exactly the twitch this is here to stop.
      if (std::fabs(cents) < kMaxPlausibleCents)
      {
        if (!mHasCents)
        {
          // First reading after silence: start there rather than sliding in from wherever the
          // filter was left by the last note.
          mSmoothedCents = cents;
          mHasCents = true;
        }
        else
        {
          // Smoothed against elapsed time rather than per block, so the needle behaves the same
          // at a 64 frame buffer as at 512 - a fixed per-block coefficient is four times faster
          // at one than the other.
          const double alpha = 1.0 - std::exp(-blockSeconds / kCentsSmoothingSeconds);
          mSmoothedCents += alpha * (cents - mSmoothedCents);
        }

        mCentsError.store(static_cast<float>(std::clamp(mSmoothedCents, -100.0, 100.0)),
                          std::memory_order_relaxed);
      }
    }
  }

  mPreviousPhase = phase;
  mHasPreviousPhase = true;
}

} // namespace nam_ui
