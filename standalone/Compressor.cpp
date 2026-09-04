#include "Compressor.h"

#include <algorithm>
#include <cmath>

namespace nam_ui
{

namespace
{
/// Slow enough that it rides over the pick attack rather than clamping it, which is what leaves an
/// optical compressor its transients.
constexpr double kAttackSeconds = 0.010;

/// The two ends of the program-dependent release. A short peak recovers at the fast one; sustained
/// work drags it toward the slow one.
constexpr double kFastReleaseSeconds = 0.080;
constexpr double kSlowReleaseSeconds = 1.200;

/// How quickly the compressor forgets that it has been working. Deliberately slower than the fast
/// release, or the release time would jump about as fast as the gain does.
constexpr double kMemorySeconds = 0.400;

/// Sustained reduction, in dB, at which the release has slowed all the way down.
constexpr double kFullWorkDb = 10.0;

/// Width of the soft knee in dB, centred on the threshold. Wide, because gradual is the point.
constexpr double kKneeDb = 8.0;

/// Below this the input is silence as far as the detector is concerned; taking its logarithm would
/// otherwise run off toward negative infinity.
constexpr double kFloor = 1.0e-6;

double OnePoleCoeff(double seconds, double sampleRate)
{
  if (seconds <= 0.0)
    return 1.0;
  return 1.0 - std::exp(-1.0 / (seconds * sampleRate));
}
} // namespace

void Compressor::Prepare(double sampleRate)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

  mAttackCoeff = OnePoleCoeff(kAttackSeconds, mSampleRate);
  mFastReleaseCoeff = OnePoleCoeff(kFastReleaseSeconds, mSampleRate);
  mSlowReleaseCoeff = OnePoleCoeff(kSlowReleaseSeconds, mSampleRate);
  mMemoryCoeff = OnePoleCoeff(kMemorySeconds, mSampleRate);

  mGainReductionDb = 0.0;
  mWorkMemoryDb = 0.0;
}

void Compressor::SetParams(float peak, bool limit)
{
  // The knob reads as "how much", so it lowers the threshold rather than raising it: at 0 nothing
  // reaches it, at 10 it sits low enough that ordinary playing is under compression throughout.
  const double clamped = std::clamp(static_cast<double>(peak), 0.0, 10.0);
  mThresholdDb = -4.0 * clamped;

  // Compress against limit, the two positions of the switch. Limiting is not infinite here - a
  // real optical limiter is not either, and 20:1 keeps it musical rather than brittle.
  const double ratio = limit ? 20.0 : 3.0;
  mSlope = 1.0 - 1.0 / ratio;
}

void Compressor::Process(NAM_SAMPLE* buffer, unsigned int frames)
{
  for (unsigned int i = 0; i < frames; i++)
  {
    const double sample = static_cast<double>(buffer[i]);
    const double magnitude = std::max(std::fabs(sample), kFloor);
    const double levelDb = 20.0 * std::log10(magnitude);

    // Soft knee: nothing below it, the full slope above it, and a quadratic bridge between - which
    // is what stops the compression announcing itself as it engages.
    const double overDb = levelDb - mThresholdDb;
    double targetDb = 0.0;
    if (overDb >= kKneeDb * 0.5)
      targetDb = mSlope * overDb;
    else if (overDb > -kKneeDb * 0.5)
    {
      const double intoKnee = overDb + kKneeDb * 0.5;
      targetDb = mSlope * intoKnee * intoKnee / (2.0 * kKneeDb);
    }

    // How long it has been working, which is what sets how slowly it lets go.
    const double memoryCoeff = (targetDb > mWorkMemoryDb) ? mAttackCoeff : mMemoryCoeff;
    mWorkMemoryDb += (targetDb - mWorkMemoryDb) * memoryCoeff;

    double coeff;
    if (targetDb > mGainReductionDb)
    {
      coeff = mAttackCoeff;
    }
    else
    {
      const double work = std::clamp(mWorkMemoryDb / kFullWorkDb, 0.0, 1.0);
      coeff = mFastReleaseCoeff + (mSlowReleaseCoeff - mFastReleaseCoeff) * work;
    }
    mGainReductionDb += (targetDb - mGainReductionDb) * coeff;

    buffer[i] = static_cast<NAM_SAMPLE>(sample * std::pow(10.0, -mGainReductionDb / 20.0));
  }
}

} // namespace nam_ui
