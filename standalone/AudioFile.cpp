#include "AudioFile.h"

#include <algorithm>
#include <cmath>

// The one place these are compiled. Every other file that wants them includes the headers plain.
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

namespace nam_ui
{

namespace
{

std::string LowerExtension(const std::filesystem::path& path)
{
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

/// Takes whatever channel count the decoder gave us down to interleaved stereo. Mono is copied to
/// both sides; anything wider keeps the first two channels, which is what a stereo mix of a
/// surround file would be anyway.
void ToStereo(const std::vector<float>& source, unsigned int channels, std::vector<float>& out)
{
  if (channels == 0)
    return;

  const size_t frames = source.size() / channels;
  out.resize(frames * 2);

  if (channels == 1)
  {
    for (size_t i = 0; i < frames; i++)
    {
      out[i * 2 + 0] = source[i];
      out[i * 2 + 1] = source[i];
    }
    return;
  }

  for (size_t i = 0; i < frames; i++)
  {
    out[i * 2 + 0] = source[i * channels + 0];
    out[i * 2 + 1] = source[i * channels + 1];
  }
}

/// Linear resampling of one interleaved stereo buffer. Linear rather than something better on
/// purpose: this runs once per file rather than per block, but a windowed sinc over a five minute
/// track is seconds of waiting for a difference nobody hears on a practice loop.
void ResampleStereo(const std::vector<float>& source, double fromRate, double toRate, std::vector<float>& out)
{
  if (source.empty() || fromRate <= 0.0 || toRate <= 0.0)
  {
    out = source;
    return;
  }
  if (std::fabs(fromRate - toRate) < 0.5)
  {
    out = source;
    return;
  }

  const size_t inFrames = source.size() / 2;
  const double ratio = fromRate / toRate;
  const size_t outFrames = static_cast<size_t>(static_cast<double>(inFrames) / ratio);
  out.assign(outFrames * 2, 0.0f);

  for (size_t i = 0; i < outFrames; i++)
  {
    const double position = static_cast<double>(i) * ratio;
    const size_t index = static_cast<size_t>(position);
    const float fraction = static_cast<float>(position - static_cast<double>(index));
    const size_t next = std::min(index + 1, inFrames - 1);

    out[i * 2 + 0] = source[index * 2 + 0] + (source[next * 2 + 0] - source[index * 2 + 0]) * fraction;
    out[i * 2 + 1] = source[index * 2 + 1] + (source[next * 2 + 1] - source[index * 2 + 1]) * fraction;
  }
}

} // namespace

bool AudioFile::IsSupported(const std::filesystem::path& path)
{
  const std::string ext = LowerExtension(path);
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

bool AudioFile::Write(const std::filesystem::path& path, const AudioFile& file, std::string& error)
{
  error.clear();

  if (file.samples.empty())
  {
    error = "Nothing to write.";
    return false;
  }

  drwav_data_format format{};
  format.container = drwav_container_riff;
  format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  format.channels = 2;
  format.sampleRate = static_cast<drwav_uint32>(file.sampleRate);
  format.bitsPerSample = 32;

  drwav wav;
  if (!drwav_init_file_write(&wav, path.string().c_str(), &format, nullptr))
  {
    error = "Could not write " + path.string();
    return false;
  }

  const drwav_uint64 frames = static_cast<drwav_uint64>(file.FrameCount());
  const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, file.samples.data());
  drwav_uninit(&wav);

  if (written != frames)
  {
    error = "Could not finish writing " + path.string();
    return false;
  }
  return true;
}

bool AudioFile::Load(const std::filesystem::path& path, double targetSampleRate, AudioFile& out, std::string& error)
{
  error.clear();

  const std::string ext = LowerExtension(path);
  const std::string filename = path.string();

  std::vector<float> decoded;
  unsigned int channels = 0;
  unsigned int fileRate = 0;

  if (ext == ".wav")
  {
    drwav_uint64 frames = 0;
    float* data = drwav_open_file_and_read_pcm_frames_f32(filename.c_str(), &channels, &fileRate, &frames, nullptr);
    if (data == nullptr)
    {
      error = "Could not read that WAV file.";
      return false;
    }
    decoded.assign(data, data + frames * channels);
    drwav_free(data, nullptr);
  }
  else if (ext == ".mp3")
  {
    drmp3_config config;
    drmp3_uint64 frames = 0;
    float* data = drmp3_open_file_and_read_pcm_frames_f32(filename.c_str(), &config, &frames, nullptr);
    if (data == nullptr)
    {
      error = "Could not read that MP3.";
      return false;
    }
    channels = config.channels;
    fileRate = config.sampleRate;
    decoded.assign(data, data + frames * channels);
    drmp3_free(data, nullptr);
  }
  else if (ext == ".flac")
  {
    drflac_uint64 frames = 0;
    float* data = drflac_open_file_and_read_pcm_frames_f32(filename.c_str(), &channels, &fileRate, &frames, nullptr);
    if (data == nullptr)
    {
      error = "Could not read that FLAC.";
      return false;
    }
    decoded.assign(data, data + frames * channels);
    drflac_free(data, nullptr);
  }
  else
  {
    error = "Only .wav, .mp3 and .flac can be opened.";
    return false;
  }

  if (decoded.empty() || channels == 0 || fileRate == 0)
  {
    error = "That file decoded to nothing.";
    return false;
  }

  std::vector<float> stereo;
  ToStereo(decoded, channels, stereo);
  decoded.clear();
  decoded.shrink_to_fit(); // a long track is hundreds of megabytes; do not hold two copies

  out.path = path;
  out.name = path.stem().string();
  out.sourceSampleRate = static_cast<double>(fileRate);
  out.sampleRate = targetSampleRate > 0.0 ? targetSampleRate : static_cast<double>(fileRate);
  ResampleStereo(stereo, static_cast<double>(fileRate), out.sampleRate, out.samples);

  return !out.samples.empty();
}

void AudioFile::PeakBetween(size_t fromFrame, size_t toFrame, float& low, float& high) const
{
  low = 0.0f;
  high = 0.0f;

  const size_t frames = FrameCount();
  if (frames == 0)
    return;

  const size_t first = std::min(fromFrame, frames - 1);
  const size_t last = std::min(std::max(toFrame, first + 1), frames);

  float lowest = 1.0f;
  float highest = -1.0f;
  for (size_t frame = first; frame < last; frame++)
  {
    const float left = samples[frame * 2 + 0];
    const float right = samples[frame * 2 + 1];
    lowest = std::min({lowest, left, right});
    highest = std::max({highest, left, right});
  }

  low = std::min(lowest, 0.0f);
  high = std::max(highest, 0.0f);
}

void PeakCache::Clear()
{
  mPeaks.clear();
}

void PeakCache::Build(const AudioFile& file, size_t buckets)
{
  mPeaks.clear();
  const size_t frames = file.FrameCount();
  if (frames == 0 || buckets == 0)
    return;

  // Never more buckets than frames: past that each bucket would hold less than a sample and the
  // waveform would be drawn from nothing.
  const size_t count = std::min(buckets, frames);
  mPeaks.resize(count);

  for (size_t bucket = 0; bucket < count; bucket++)
  {
    const size_t from = frames * bucket / count;
    const size_t to = std::max(from + 1, frames * (bucket + 1) / count);

    float low = 1.0f;
    float high = -1.0f;
    for (size_t frame = from; frame < to && frame < frames; frame++)
    {
      // Both channels together: a waveform is a picture of the file, not of one side of it.
      const float left = file.samples[frame * 2 + 0];
      const float right = file.samples[frame * 2 + 1];
      low = std::min({low, left, right});
      high = std::max({high, left, right});
    }

    mPeaks[bucket].low = std::min(low, 0.0f);
    mPeaks[bucket].high = std::max(high, 0.0f);
  }
}

PeakCache::Peak PeakCache::Range(double from, double to) const
{
  if (mPeaks.empty())
    return {};

  const double count = static_cast<double>(mPeaks.size());
  const size_t first = static_cast<size_t>(std::clamp(from, 0.0, 1.0) * (count - 1.0));
  const size_t last = static_cast<size_t>(std::clamp(to, 0.0, 1.0) * (count - 1.0));

  Peak peak{1.0f, -1.0f};
  for (size_t i = first; i <= last && i < mPeaks.size(); i++)
  {
    peak.low = std::min(peak.low, mPeaks[i].low);
    peak.high = std::max(peak.high, mPeaks[i].high);
  }

  if (peak.high < peak.low)
    return {};
  return peak;
}

} // namespace nam_ui
