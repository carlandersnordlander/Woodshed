#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nam_ui
{

/// \brief One decoded audio file, held in memory as interleaved stereo at the stream's rate.
///
/// A track is minutes long rather than milliseconds, so it is decoded and resampled once at load
/// and then only read from. Five minutes of stereo float at 48 kHz is about 110 MB, which is a
/// price worth paying for playback that never touches the disk or the decoder again.
struct AudioFile
{
  std::filesystem::path path;
  std::string name;

  /// Interleaved left, right. Always two channels, whatever the source had.
  std::vector<float> samples;
  double sampleRate = 48000.0;
  /// The rate the file was written at, for showing rather than for playing.
  double sourceSampleRate = 0.0;

  size_t FrameCount() const { return samples.size() / 2; }

  /// Loudest and quietest sample between two frames, read straight from the file. Used when the
  /// view is zoomed in past what the peak cache can resolve.
  void PeakBetween(size_t fromFrame, size_t toFrame, float& low, float& high) const;
  double DurationSeconds() const { return sampleRate > 0.0 ? static_cast<double>(FrameCount()) / sampleRate : 0.0; }
  bool Empty() const { return samples.empty(); }

  /// \brief Decodes a .wav, .mp3 or .flac and resamples it to `targetSampleRate`.
  ///
  /// \param error set to something worth showing a person when this returns false
  static bool Load(const std::filesystem::path& path, double targetSampleRate, AudioFile& out, std::string& error);

  /// The extensions Load() understands, for file dialogs and for filtering a folder.
  static bool IsSupported(const std::filesystem::path& path);

  /// \brief Writes what is in memory back out as a 32-bit float wav.
  ///
  /// Only needed to hand audio to a tool that reads files rather than buffers - a model that wants
  /// the whole mix when the mix only exists as several separated stems, say. Float rather than
  /// 16-bit because nothing here is going to be listened to; it is going to be analysed, and a
  /// conversion that can clip has no place in the middle of that.
  static bool Write(const std::filesystem::path& path, const AudioFile& file, std::string& error);
};

/// \brief Min and max per horizontal pixel, which is what a waveform actually is.
///
/// Drawing a five minute file by walking every sample would be fourteen million samples a frame.
/// This is built once on load, off the UI thread, and drawn from thereafter.
class PeakCache
{
public:
  struct Peak
  {
    float low = 0.0f;
    float high = 0.0f;
  };

  /// \param buckets how many columns to reduce the file to. Far more than any window is wide, so
  ///        moderate zoom stays smooth without rebuilding; past that the file is read directly.
  ///        65536 buckets is half a megabyte per track, which is nothing next to the audio.
  void Build(const AudioFile& file, size_t buckets = 65536);
  void Clear();

  bool Empty() const { return mPeaks.empty(); }
  size_t Size() const { return mPeaks.size(); }

  /// The loudest and quietest sample between two positions in the file, given as fractions 0..1.
  Peak Range(double from, double to) const;

private:
  std::vector<Peak> mPeaks;
};

} // namespace nam_ui
