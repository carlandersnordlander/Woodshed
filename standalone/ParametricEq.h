#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "RecursiveLinearFilter.h"

namespace nam_ui
{

/// How many bands a parametric EQ block has.
constexpr size_t kEqBandCount = 5;

constexpr float kBandMinHz = 20.0f;
constexpr float kBandMaxHz = 20000.0f;
constexpr float kBandMinGainDb = -18.0f;
constexpr float kBandMaxGainDb = 18.0f;
constexpr float kBandMinQ = 0.3f;
constexpr float kBandMaxQ = 8.0f;

/// One peaking band. All five are peaking rather than the usual shelves at the ends: a shelf has
/// no Q worth dragging, and the point of this block is that every band is the same thing.
struct EqBand
{
  bool enabled = true;
  float hz = 1000.0f;
  float gainDb = 0.0f;
  float q = 1.0f;

  bool operator==(const EqBand& other) const
  {
    return enabled == other.enabled && hz == other.hz && gainDb == other.gainDb && q == other.q;
  }
  bool operator!=(const EqBand& other) const { return !(*this == other); }
};

struct ParametricEqSettings
{
  std::array<EqBand, kEqBandCount> bands{};

  ParametricEqSettings();

  bool operator==(const ParametricEqSettings& other) const { return bands == other.bands; }
  bool operator!=(const ParametricEqSettings& other) const { return !(*this == other); }
};

/// One biquad's coefficients, already divided through by a0 so the difference equation is
/// y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2.
struct BiquadCoefficients
{
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
};

/// Audio EQ Cookbook peaking filter. One function, used by everything that either runs a band or
/// draws one, so a filter and its curve are always the same filter.
BiquadCoefficients PeakingCoefficients(const EqBand& band, double sampleRate);

/// \brief Five peaking biquads in series, and the maths to draw what they do.
///
/// The response is computed from the same numbers that drive the filters rather than measured from
/// them, so the curve on screen and the sound cannot drift apart.
class ParametricEq
{
public:
  void Prepare(double sampleRate, int maxFrames);
  void ApplyIfChanged(const ParametricEqSettings& settings);
  DSP_SAMPLE* Process(DSP_SAMPLE** input, size_t numFrames);

  /// Combined response of every enabled band at one frequency, in dB. Static so the UI can draw a
  /// curve for settings that are not loaded into any running filter.
  static float ResponseDb(const ParametricEqSettings& settings, float hz, double sampleRate);

private:
  /// One peaking band's response, in dB, from the Audio EQ Cookbook coefficients.
  static float BandResponseDb(const EqBand& band, float hz, double sampleRate);

  std::array<recursive_linear_filter::Peaking, kEqBandCount> mBands;
  double mSampleRate = 48000.0;
  ParametricEqSettings mApplied;
  bool mHasApplied = false;
};

/// \brief The same five bands, run over an interleaved stereo pair.
///
/// The block chain is mono and uses ParametricEq above; a backing track is stereo and is mixed
/// sample by sample rather than in channel buffers, so it needs a filter shaped to that. Both are
/// built from PeakingCoefficients, so the two spellings of "five peaking bands" cannot disagree.
class StereoParametricEq
{
public:
  /// Rebuilds the coefficients if the settings changed. Cheap enough for the audio thread, and it
  /// only ever does the work while a handle is being dragged.
  void SetSettings(const ParametricEqSettings& settings, double sampleRate);
  /// Zeroes the delay lines. Called when a slot is handed to a different track, so it does not
  /// ring with the last one's tail.
  void Reset();

  /// One frame, in place. Audio thread.
  void ProcessSample(float& left, float& right);

private:
  struct State
  {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
  };

  std::array<BiquadCoefficients, kEqBandCount> mCoefficients{};
  std::array<State, kEqBandCount> mLeft{};
  std::array<State, kEqBandCount> mRight{};
  ParametricEqSettings mApplied;
  double mSampleRate = 48000.0;
  bool mHasApplied = false;
};

/// \brief Collects input samples on the audio thread for the UI to run an FFT over.
///
/// A single ring buffer with a relaxed write index. The reader may occasionally catch a block
/// being overwritten mid-copy; on a spectrum display that costs one slightly wrong frame out of
/// sixty and is not worth a lock on the audio thread to avoid.
class SpectrumTap
{
public:
  /// Power of two. 2048 at 48 kHz is a ~23 Hz resolution and a 43 ms window - fine enough to see
  /// where the low end sits, short enough that the display still tracks playing.
  static constexpr size_t kSize = 2048;

  /// Audio thread. Copies whatever fits; never blocks.
  void Write(const float* samples, unsigned int frames);

  /// UI thread. Fills `out` with the most recent kSize samples, oldest first.
  void Read(std::vector<float>& out) const;

private:
  std::array<float, kSize> mBuffer{};
  std::atomic<size_t> mWritePosition{0};
};

/// In-place radix-2 FFT on interleaved real/imaginary arrays. Small enough to keep here rather
/// than pull in a library for one spectrum display.
void ForwardFft(std::vector<float>& real, std::vector<float>& imag);

} // namespace nam_ui
