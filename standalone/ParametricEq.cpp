#include "ParametricEq.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace nam_ui
{

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// Spread across the band so the five handles do not start stacked on top of each other.
constexpr float kDefaultHz[kEqBandCount] = {80.0f, 240.0f, 800.0f, 2500.0f, 7000.0f};
} // namespace

ParametricEqSettings::ParametricEqSettings()
{
  for (size_t i = 0; i < kEqBandCount; i++)
  {
    bands[i].hz = kDefaultHz[i];
    bands[i].gainDb = 0.0f;
    bands[i].q = 1.0f;
    bands[i].enabled = true;
  }
}

void ParametricEq::Prepare(double sampleRate, int maxFrames)
{
  mSampleRate = sampleRate;
  mHasApplied = false;

  // One silent block at the largest size we will ever see, so the filters size their buffers now
  // and Process() never allocates on the audio thread afterwards.
  const size_t frames = static_cast<size_t>(std::max(maxFrames, 1));
  std::vector<DSP_SAMPLE> silence(frames, DSP_SAMPLE(0));
  DSP_SAMPLE* channel = silence.data();
  DSP_SAMPLE** input = &channel;

  ApplyIfChanged(ParametricEqSettings());
  Process(input, frames);

  mHasApplied = false;
}

void ParametricEq::ApplyIfChanged(const ParametricEqSettings& settings)
{
  if (mHasApplied && settings == mApplied)
    return;

  for (size_t i = 0; i < kEqBandCount; i++)
  {
    const EqBand& band = settings.bands[i];
    // A disabled band is flat rather than skipped: keeping it in the chain means enabling it does
    // not jump, and a peaking filter at 0 dB passes the signal through unchanged anyway.
    const double gain = band.enabled ? static_cast<double>(band.gainDb) : 0.0;
    mBands[i].SetParams(recursive_linear_filter::BiquadParams(
      mSampleRate, std::clamp(static_cast<double>(band.hz), 20.0, mSampleRate * 0.49),
      std::clamp(static_cast<double>(band.q), static_cast<double>(kBandMinQ), static_cast<double>(kBandMaxQ)), gain));
  }

  mApplied = settings;
  mHasApplied = true;
}

DSP_SAMPLE* ParametricEq::Process(DSP_SAMPLE** input, size_t numFrames)
{
  DSP_SAMPLE** current = input;
  for (auto& band : mBands)
    current = band.Process(current, 1, numFrames);
  return current[0];
}

BiquadCoefficients PeakingCoefficients(const EqBand& band, double sampleRate)
{
  BiquadCoefficients out;
  if (!band.enabled || band.gainDb == 0.0f)
    return out; // b0 = 1, everything else zero: a straight wire

  const double a = std::pow(10.0, static_cast<double>(band.gainDb) / 40.0);
  const double w0 = 2.0 * kPi * std::clamp(static_cast<double>(band.hz), 20.0, sampleRate * 0.49) / sampleRate;
  const double alpha = std::sin(w0) / (2.0 * std::clamp(static_cast<double>(band.q), 0.05, 20.0));
  const double cosw0 = std::cos(w0);

  const double a0 = 1.0 + alpha / a;
  out.b0 = (1.0 + alpha * a) / a0;
  out.b1 = (-2.0 * cosw0) / a0;
  out.b2 = (1.0 - alpha * a) / a0;
  out.a1 = (-2.0 * cosw0) / a0;
  out.a2 = (1.0 - alpha / a) / a0;
  return out;
}

float ParametricEq::BandResponseDb(const EqBand& band, float hz, double sampleRate)
{
  if (!band.enabled || band.gainDb == 0.0f)
    return 0.0f;

  // The filter's own coefficients, evaluated on the unit circle - so the drawn curve is the filter
  // rather than an impression of it.
  const BiquadCoefficients c = PeakingCoefficients(band, sampleRate);

  const double w = 2.0 * kPi * static_cast<double>(hz) / sampleRate;
  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> z2 = std::polar(1.0, -2.0 * w);

  const std::complex<double> numerator = c.b0 + c.b1 * z1 + c.b2 * z2;
  const std::complex<double> denominator = 1.0 + c.a1 * z1 + c.a2 * z2;
  const double magnitude = std::abs(numerator / denominator);

  return static_cast<float>(20.0 * std::log10(std::max(magnitude, 1.0e-9)));
}

void StereoParametricEq::SetSettings(const ParametricEqSettings& settings, double sampleRate)
{
  if (mHasApplied && sampleRate == mSampleRate && settings == mApplied)
    return;

  for (size_t i = 0; i < kEqBandCount; i++)
    mCoefficients[i] = PeakingCoefficients(settings.bands[i], sampleRate);

  mApplied = settings;
  mSampleRate = sampleRate;
  mHasApplied = true;
}

void StereoParametricEq::Reset()
{
  mLeft = {};
  mRight = {};
}

void StereoParametricEq::ProcessSample(float& left, float& right)
{
  double l = static_cast<double>(left);
  double r = static_cast<double>(right);

  for (size_t i = 0; i < kEqBandCount; i++)
  {
    const BiquadCoefficients& c = mCoefficients[i];

    State& sl = mLeft[i];
    const double outL = c.b0 * l + c.b1 * sl.x1 + c.b2 * sl.x2 - c.a1 * sl.y1 - c.a2 * sl.y2;
    sl.x2 = sl.x1;
    sl.x1 = l;
    sl.y2 = sl.y1;
    sl.y1 = outL;
    l = outL;

    State& sr = mRight[i];
    const double outR = c.b0 * r + c.b1 * sr.x1 + c.b2 * sr.x2 - c.a1 * sr.y1 - c.a2 * sr.y2;
    sr.x2 = sr.x1;
    sr.x1 = r;
    sr.y2 = sr.y1;
    sr.y1 = outR;
    r = outR;
  }

  left = static_cast<float>(l);
  right = static_cast<float>(r);
}

float ParametricEq::ResponseDb(const ParametricEqSettings& settings, float hz, double sampleRate)
{
  // Filters in series multiply, so their dB responses add.
  float total = 0.0f;
  for (const auto& band : settings.bands)
    total += BandResponseDb(band, hz, sampleRate);
  return total;
}

void SpectrumTap::Write(const float* samples, unsigned int frames)
{
  size_t position = mWritePosition.load(std::memory_order_relaxed);
  for (unsigned int i = 0; i < frames; i++)
  {
    mBuffer[position] = samples[i];
    position = (position + 1) & (kSize - 1);
  }
  mWritePosition.store(position, std::memory_order_release);
}

void SpectrumTap::Read(std::vector<float>& out) const
{
  out.resize(kSize);
  const size_t position = mWritePosition.load(std::memory_order_acquire);
  for (size_t i = 0; i < kSize; i++)
    out[i] = mBuffer[(position + i) & (kSize - 1)];
}

void ForwardFft(std::vector<float>& real, std::vector<float>& imag)
{
  const size_t n = real.size();
  if (n < 2 || (n & (n - 1)) != 0)
    return;

  // Bit-reversal permutation.
  for (size_t i = 1, j = 0; i < n; i++)
  {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
    {
      std::swap(real[i], real[j]);
      std::swap(imag[i], imag[j]);
    }
  }

  for (size_t length = 2; length <= n; length <<= 1)
  {
    const double angle = -2.0 * kPi / static_cast<double>(length);
    const float stepRe = static_cast<float>(std::cos(angle));
    const float stepIm = static_cast<float>(std::sin(angle));

    for (size_t start = 0; start < n; start += length)
    {
      float wRe = 1.0f;
      float wIm = 0.0f;
      for (size_t k = 0; k < length / 2; k++)
      {
        const size_t even = start + k;
        const size_t odd = even + length / 2;

        const float tRe = real[odd] * wRe - imag[odd] * wIm;
        const float tIm = real[odd] * wIm + imag[odd] * wRe;

        real[odd] = real[even] - tRe;
        imag[odd] = imag[even] - tIm;
        real[even] += tRe;
        imag[even] += tIm;

        const float nextRe = wRe * stepRe - wIm * stepIm;
        wIm = wRe * stepIm + wIm * stepRe;
        wRe = nextRe;
      }
    }
  }
}

} // namespace nam_ui
