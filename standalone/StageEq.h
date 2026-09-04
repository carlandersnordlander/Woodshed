#pragma once

#include "RecursiveLinearFilter.h"

namespace nam_ui
{

/// Where a stage's EQ sits relative to its capture. Pre shapes what the capture is fed - which
/// changes how it distorts - while Post shapes the result, leaving the drive untouched.
enum class EqPlacement
{
  Pre = 0,
  Post = 1
};

struct EqSettings
{
  bool enabled = false;
  EqPlacement placement = EqPlacement::Post;
  float lowDb = 0.0f;
  float midDb = 0.0f;
  float highDb = 0.0f;
  float midHz = 700.0f;

  bool operator==(const EqSettings& other) const
  {
    return enabled == other.enabled && placement == other.placement && lowDb == other.lowDb && midDb == other.midDb
           && highDb == other.highDb && midHz == other.midHz;
  }
  bool operator!=(const EqSettings& other) const { return !(*this == other); }
};

/// Three-band EQ built from AudioDSPTools' Audio EQ Cookbook biquads - the same low shelf,
/// peaking and high shelf the official NAM plugin uses for its tone stack.
///
/// Coefficients are recomputed on the audio thread when the settings change, rather than being
/// written from the UI thread while the filters are running.
class StageEq
{
public:
  /// Sizes the internal buffers once, so Process() never allocates afterwards.
  void Prepare(double sampleRate, int maxFrames);

  /// Recomputes coefficients if `settings` differ from what is currently applied.
  void ApplyIfChanged(const EqSettings& settings);

  /// Runs the three bands in series. Returns the buffer holding the result for channel 0.
  DSP_SAMPLE* Process(DSP_SAMPLE** input, size_t numFrames);

private:
  recursive_linear_filter::LowShelf mLow;
  recursive_linear_filter::Peaking mMid;
  recursive_linear_filter::HighShelf mHigh;

  double mSampleRate = 48000.0;
  EqSettings mApplied;
  bool mHasApplied = false;
};

/// Fixed corner frequencies. The mid is swept by the user; the shelves sit low and high enough to
/// be useful for bass as well as guitar.
constexpr double kEqLowHz = 120.0;
constexpr double kEqHighHz = 2400.0;
constexpr double kEqQuality = 0.707;
constexpr float kEqMinGainDb = -15.0f;
constexpr float kEqMaxGainDb = 15.0f;
constexpr float kEqMinMidHz = 150.0f;
constexpr float kEqMaxMidHz = 4000.0f;

} // namespace nam_ui
