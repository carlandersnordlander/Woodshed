#pragma once

#include "NAM/dsp.h" // NAM_SAMPLE

namespace nam_ui
{

/// \brief A feed-forward compressor with the manners of an optical one.
///
/// Two things give an LA-2A its character, and both are here. The knee is soft, so compression
/// arrives gradually rather than switching on at a threshold. And the release is *program
/// dependent*: it lets go quickly after a short peak, but the longer it has been working the
/// slower it recovers. That is what makes an optical compressor sit under a part instead of
/// pumping with it, and it is why a single "release" control would not reproduce it.
///
/// Everything is one-pole smoothing on a gain reduction figure in dB - no lookahead, no
/// allocation, safe to call from the audio thread.
class Compressor
{
public:
  /// Sets the sample rate and clears the envelope. Not audio-thread safe.
  void Prepare(double sampleRate);

  /// \param peak how hard it works, 0-10, the way the pedal's Peak Reduction knob reads: 0 leaves
  ///        the signal alone, 10 lowers the threshold to where quiet playing is compressed
  /// \param limit true for the limiting ratio rather than the gentler compressing one
  void SetParams(float peak, bool limit);

  /// Compresses in place. Makeup gain and any dry blend belong to the caller - the block already
  /// has controls for both, and duplicating them here would be two knobs doing one job.
  void Process(NAM_SAMPLE* buffer, unsigned int frames);

  /// How much it is pulling down right now, in dB and positive. For the meter, so the amount of
  /// compression is something you can see rather than guess at.
  float GetGainReductionDb() const { return static_cast<float>(mGainReductionDb); }

private:
  double mSampleRate = 48000.0;

  // Coefficients, rebuilt when the sample rate changes.
  double mAttackCoeff = 0.0;
  double mFastReleaseCoeff = 0.0;
  double mSlowReleaseCoeff = 0.0;
  double mMemoryCoeff = 0.0;

  double mThresholdDb = 0.0;
  double mSlope = 0.0; ///< 1 - 1/ratio, the fraction of anything above the threshold that is removed

  double mGainReductionDb = 0.0;
  /// A slow memory of how hard it has been working, which is what the release time is drawn from.
  double mWorkMemoryDb = 0.0;
};

} // namespace nam_ui
