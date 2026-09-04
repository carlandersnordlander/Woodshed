#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

namespace nam_ui
{

/// Strobe bands: the reference itself, then its octave and double octave. A real strobe tuner
/// shows several rings for the same reason - the fast ones resolve the last cent, the slow one
/// tells you which way to go when you are far off.
constexpr size_t kStrobeBandCount = 3;

/// A digital strobe tuner.
///
/// The input is multiplied by a complex oscillator running at the reference frequency and
/// smoothed, which shifts that frequency down to DC. What is left rotates at exactly the
/// difference between the played pitch and the reference, so a pattern offset by its phase stands
/// still when the note is in tune and drifts at the beat rate when it is not. That is the same
/// principle as a mechanical strobe, and the reason strobes resolve fractions of a cent: the
/// error is integrated over time rather than measured from a single frequency estimate.
class Tuner
{
public:
  /// Sets the sample rate and clears the analysis state. Not audio-thread safe.
  void Prepare(double sampleRate);

  /// The pitch being tuned to, in Hz. Safe to call from the UI thread.
  void SetReferenceHz(double hz);
  double GetReferenceHz() const { return mReferenceHz.load(std::memory_order_relaxed); }

  /// Analyses one block. Audio thread only; allocates nothing.
  void Process(const float* input, unsigned int frames);

  /// Rotation of a band, in radians. The UI offsets its pattern by this.
  float GetBandPhase(size_t band) const;
  /// How much energy sits at that band, 0..1-ish, for fading bands that have nothing to show.
  float GetBandStrength(size_t band) const;

  /// Signed error against the reference, in cents. Only meaningful when HasSignal() is true.
  float GetCentsError() const { return mCentsError.load(std::memory_order_relaxed); }
  bool HasSignal() const { return mHasSignal.load(std::memory_order_relaxed); }

  /// \brief The pitch it works out on its own, in Hz, or 0 when nothing is being played clearly.
  ///
  /// The strobe can only compare against a note it has been given, which is why a tuner built on
  /// one alone has to be told which string you are about to play. This is a separate, much coarser
  /// estimate - good to a few cents, where the strobe is good to a fraction of one - and its only
  /// job is to answer "which note is this", so the reference can be chosen for you.
  double GetDetectedHz() const { return mDetectedHz.load(std::memory_order_relaxed); }

private:
  /// Runs the pitch estimate over what is in the decimated buffer. Audio thread; no allocation.
  void Detect();

  /// Working rate for the detector. Fundamentals here top out around 700 Hz, so a few kilohertz is
  /// generous, and every halving is half the work.
  static constexpr double kDetectRateTarget = 6000.0;
  /// Enough for the comparison window plus the longest lag: 512 + 256 at 6 kHz reaches down to
  /// 23 Hz, below the lowest string anyone tunes.
  static constexpr size_t kDetectSize = 1024;
  static constexpr size_t kDetectWindow = 512;
  static constexpr size_t kDetectMaxLag = 256;

  struct Band
  {
    // Recursive complex rotator: cheaper than calling sin/cos per sample, renormalised once per
    // block so it cannot drift off the unit circle.
    double oscRe = 1.0;
    double oscIm = 0.0;
    double stepRe = 1.0;
    double stepIm = 0.0;

    // Smoothed heterodyne result; its argument is the strobe phase.
    double accRe = 0.0;
    double accIm = 0.0;

    std::atomic<float> phase{0.0f};
    std::atomic<float> strength{0.0f};
  };

  std::array<Band, kStrobeBandCount> mBands;

  std::atomic<double> mReferenceHz{110.0};
  std::atomic<float> mCentsError{0.0f};
  std::atomic<bool> mHasSignal{false};

  double mSampleRate = 48000.0;
  double mAppliedReferenceHz = 0.0;
  double mSmoothing = 0.0;

  // Cents come from how fast the fundamental's phase rotates, so the previous block's phase has
  // to be remembered.
  double mPreviousPhase = 0.0;
  bool mHasPreviousPhase = false;
  double mSmoothedCents = 0.0;
  /// False until the first plausible reading of a note, so the smoother starts at that reading
  /// instead of sliding in from what the previous note left behind.
  bool mHasCents = false;

  // --- the note detector -------------------------------------------------------------------
  //
  // A ring of low-passed, decimated samples and a YIN estimate over it. Fixed arrays: this runs on
  // the audio thread, where allocating is not allowed.
  std::array<float, kDetectSize> mDetect{};
  std::array<float, kDetectMaxLag + 1> mYin{};
  size_t mDetectWrite = 0;
  size_t mDetectFill = 0;
  /// How many input samples make one detector sample, and where in that count we are.
  int mDecimation = 8;
  int mDecimateCount = 0;
  double mDetectRate = 6000.0;
  /// Two one-poles ahead of the decimator, so what folds back is at least quiet.
  double mAntiAliasA = 0.0;
  double mAntiAliasB = 0.0;
  double mAntiAliasCoeff = 0.25;
  /// New detector samples since the last estimate: the analysis hop.
  size_t mSinceDetect = 0;
  std::atomic<double> mDetectedHz{0.0};
};

/// Note names in the order used by note indices 0..11 (0 = C).
extern const char* const kNoteNames[12];

/// Concert-pitch frequency of a note. `noteIndex` 0..11 with 0 = C, `octave` in scientific pitch
/// notation, so A4 with a4Hz = 440 gives 440.
double NoteFrequency(int noteIndex, int octave, double a4Hz);

/// Human-readable name, e.g. "A2".
std::string NoteName(int noteIndex, int octave);

} // namespace nam_ui
