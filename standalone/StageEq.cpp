#include "StageEq.h"

#include <algorithm>
#include <vector>

namespace nam_ui
{

void StageEq::Prepare(double sampleRate, int maxFrames)
{
  mSampleRate = sampleRate;
  mHasApplied = false; // coefficients depend on the sample rate, so force a recompute

  // Run one silent block at the largest size we will ever see. The filters size their buffers to
  // match, and std::vector keeps that capacity, so later (smaller) blocks never allocate.
  const size_t frames = static_cast<size_t>(std::max(maxFrames, 1));
  std::vector<DSP_SAMPLE> silence(frames, DSP_SAMPLE(0));
  DSP_SAMPLE* channel = silence.data();
  DSP_SAMPLE** input = &channel;

  EqSettings flat;
  flat.enabled = true;
  ApplyIfChanged(flat);
  Process(input, frames);

  mHasApplied = false; // the real settings still need applying on the first audio block
}

void StageEq::ApplyIfChanged(const EqSettings& settings)
{
  if (mHasApplied && settings == mApplied)
    return;

  mLow.SetParams(recursive_linear_filter::BiquadParams(mSampleRate, kEqLowHz, kEqQuality, settings.lowDb));
  mMid.SetParams(recursive_linear_filter::BiquadParams(
    mSampleRate, std::clamp(static_cast<double>(settings.midHz), static_cast<double>(kEqMinMidHz),
                            static_cast<double>(kEqMaxMidHz)),
    kEqQuality, settings.midDb));
  mHigh.SetParams(recursive_linear_filter::BiquadParams(mSampleRate, kEqHighHz, kEqQuality, settings.highDb));

  mApplied = settings;
  mHasApplied = true;
}

DSP_SAMPLE* StageEq::Process(DSP_SAMPLE** input, size_t numFrames)
{
  // Each filter writes into its own buffer and hands back a pointer, so the three chain directly
  // without any scratch space of ours.
  DSP_SAMPLE** low = mLow.Process(input, 1, numFrames);
  DSP_SAMPLE** mid = mMid.Process(low, 1, numFrames);
  DSP_SAMPLE** high = mHigh.Process(mid, 1, numFrames);
  return high[0];
}

} // namespace nam_ui
