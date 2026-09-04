#include "NoteAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ParametricEq.h" // ForwardFft, for the chromagram
#include "Tuner.h"

namespace nam_ui
{

namespace
{

/// \brief Everything the detector is sized around.
///
/// The signal is decimated to about 6 kHz first. Fundamentals stop at roughly 1.3 kHz - the top
/// fret of a guitar's top string - so everything above that is detail the pitch does not depend
/// on, and dropping it makes the search eight times cheaper.
constexpr double kTargetRate = 6000.0;
/// 512 samples is about 85 ms: two full periods of a low B, which is the least YIN can work with.
constexpr size_t kWindow = 512;
/// 96 samples is about 16 ms between estimates - fine enough to place an onset by eye against the
/// waveform above it.
constexpr size_t kHop = 96;
/// The lag range, and so the pitch range: about 23 Hz to 1500 Hz.
constexpr size_t kTauMin = 4;
constexpr size_t kTauMax = 256;
/// Below this a lag counts as periodic. 0.15 is the paper's value and it holds up.
constexpr double kYinThreshold = 0.15;
/// How periodic a frame has to be before it is a note rather than noise.
constexpr double kMinConfidence = 0.55;
/// Notes shorter than this are detector noise, not playing.
constexpr double kMinNoteSeconds = 0.05;
/// A pitch this far from where the note has been is a new note, not vibrato. Just over a quarter
/// tone, so a bend is one note and a semitone move is two.
constexpr double kNewNoteSemitones = 0.6;
/// Gaps shorter than this inside one note - a pick attack, a moment of noise - do not end it.
constexpr int kMaxGapFrames = 3;

/// Mono, decimated. The average over each group of samples is a crude low-pass, but its nulls sit
/// exactly on the multiples of the new rate, which is where aliasing would come from.
std::vector<float> Decimate(const AudioFile& file, double& rateOut, double targetRate = kTargetRate)
{
  const int factor = std::max(1, static_cast<int>(std::floor(file.sampleRate / targetRate)));
  rateOut = file.sampleRate / static_cast<double>(factor);

  const size_t frames = file.FrameCount();
  std::vector<float> out;
  out.reserve(frames / static_cast<size_t>(factor) + 1);

  for (size_t i = 0; i + static_cast<size_t>(factor) <= frames; i += static_cast<size_t>(factor))
  {
    float sum = 0.0f;
    for (int k = 0; k < factor; k++)
      sum += 0.5f * (file.samples[(i + static_cast<size_t>(k)) * 2 + 0] + file.samples[(i + static_cast<size_t>(k)) * 2 + 1]);
    out.push_back(sum / static_cast<float>(factor));
  }
  return out;
}

struct Frame
{
  float midi = 0.0f;
  float confidence = 0.0f;
  float rms = 0.0f;
  bool voiced = false;
};

/// One YIN estimate over `window` samples starting at `start`.
/// \return the pitch in Hz, or 0 when the window is not periodic enough to say
double EstimatePitch(const std::vector<float>& signal, size_t start, double rate, std::vector<double>& difference,
                     std::vector<double>& normalised, double& confidenceOut)
{
  // --- the difference function ---
  //
  // d(tau) = sum over the window of (x[j] - x[j+tau])^2. Small where the signal repeats itself
  // after tau samples, which is what a period is.
  for (size_t tau = 1; tau <= kTauMax; tau++)
  {
    double sum = 0.0;
    for (size_t j = 0; j < kWindow; j++)
    {
      const double delta = static_cast<double>(signal[start + j]) - static_cast<double>(signal[start + j + tau]);
      sum += delta * delta;
    }
    difference[tau] = sum;
  }

  // --- cumulative mean normalisation ---
  //
  // Each lag divided by the average of every lag up to it. Without this the difference function is
  // smallest at tau = 0 and gets smaller again at every multiple of the true period, and the
  // detector reports an octave down about as often as not.
  normalised[0] = 1.0;
  double running = 0.0;
  for (size_t tau = 1; tau <= kTauMax; tau++)
  {
    running += difference[tau];
    normalised[tau] = (running > 0.0) ? difference[tau] * static_cast<double>(tau) / running : 1.0;
  }

  // --- the first lag good enough, not the best one ---
  //
  // Taking the global minimum would again favour multiples of the period. The first dip below the
  // threshold is the fundamental; descending into it finds the bottom of that dip.
  size_t best = 0;
  for (size_t tau = kTauMin; tau <= kTauMax; tau++)
  {
    if (normalised[tau] >= kYinThreshold)
      continue;
    while (tau + 1 <= kTauMax && normalised[tau + 1] < normalised[tau])
      tau++;
    best = tau;
    break;
  }

  if (best == 0)
  {
    // Nothing was periodic enough. The lowest score still says how close it came, which is what
    // decides whether this frame is a note at all.
    double lowest = 2.0;
    for (size_t tau = kTauMin; tau <= kTauMax; tau++)
    {
      if (normalised[tau] < lowest)
      {
        lowest = normalised[tau];
        best = tau;
      }
    }
  }

  confidenceOut = std::clamp(1.0 - normalised[best], 0.0, 1.0);

  // A lag is a whole number of samples, and at 6 kHz that is a quarter of a semitone at the top of
  // the range. The parabola through the three points either side gives the rest.
  double refined = static_cast<double>(best);
  if (best > kTauMin && best < kTauMax)
  {
    const double a = normalised[best - 1];
    const double b = normalised[best];
    const double c = normalised[best + 1];
    const double denominator = a - 2.0 * b + c;
    if (std::fabs(denominator) > 1.0e-12)
      refined += std::clamp(0.5 * (a - c) / denominator, -1.0, 1.0);
  }

  return (refined > 0.0) ? rate / refined : 0.0;
}

constexpr double kPi = 3.14159265358979323846;

/// How strong one frequency is over a stretch of samples. Goertzel: a single resonator, which is
/// all that is wanted when the question is about two frequencies rather than the whole spectrum.
double ToneMagnitude(const std::vector<float>& signal, size_t from, size_t to, double hz, double rate)
{
  if (hz <= 0.0 || hz >= rate * 0.5 || from >= to || to > signal.size())
    return 0.0;

  const double coefficient = 2.0 * std::cos(2.0 * kPi * hz / rate);
  double s1 = 0.0;
  double s2 = 0.0;
  for (size_t i = from; i < to; i++)
  {
    const double s = static_cast<double>(signal[i]) + coefficient * s1 - s2;
    s2 = s1;
    s1 = s;
  }

  const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
  return std::sqrt(std::max(0.0, power)) / static_cast<double>(to - from);
}

double MidiToHz(double midi)
{
  return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

/// Median of a short run, used to keep one bad frame from splitting a note in two.
float MedianOf(std::vector<float> values)
{
  if (values.empty())
    return 0.0f;
  const size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
  return values[middle];
}

/// The lowest and highest note in a track, for laying its lane out vertically.
void MeasureRange(NoteTrack& track)
{
  if (track.notes.empty())
  {
    track.lowestMidi = 60;
    track.highestMidi = 60;
    return;
  }

  track.lowestMidi = track.notes.front().midi;
  track.highestMidi = track.notes.front().midi;
  for (const auto& note : track.notes)
  {
    track.lowestMidi = std::min(track.lowestMidi, note.midi);
    track.highestMidi = std::max(track.highestMidi, note.midi);
  }
}

} // namespace

void ApplyNoteCleanup(NoteTrack& track)
{
  std::vector<DetectedNote> notes = track.rawNotes;
  track.notes.clear();
  if (notes.empty())
  {
    MeasureRange(track);
    return;
  }

  const float strength = std::clamp(track.cleanupStrength, 0.0f, 1.0f);

  std::sort(notes.begin(), notes.end(),
            [](const DetectedNote& a, const DetectedNote& b) { return a.startSeconds < b.startSeconds; });

  std::vector<char> keep(notes.size(), 1);

  // The longest note there is bounds how far back a note that overlaps this one can have started,
  // which is what keeps the passes below from comparing every note against every other one.
  double longest = 0.0;
  for (const auto& note : notes)
    longest = std::max(longest, note.DurationSeconds());

  const auto firstThatCouldOverlap = [&](size_t i)
  {
    size_t first = i;
    while (first > 0 && notes[first - 1].startSeconds >= notes[i].startSeconds - longest)
      first--;
    return first;
  };

  // --- one note picked twice is one note ---
  //
  // A transcriber re-triggers on a re-attack, on a moment of noise inside a held note, and on the
  // point where a note it was unsure of crosses back over the threshold. Read on a screen those are
  // one note with a scratch through it.
  const double mergeGap = 0.02 + 0.08 * static_cast<double>(strength);
  for (size_t i = 0; i < notes.size(); i++)
  {
    if (!keep[i])
      continue;
    for (size_t j = i + 1; j < notes.size(); j++)
    {
      if (notes[j].startSeconds > notes[i].endSeconds + mergeGap)
        break;
      if (!keep[j] || notes[j].midi != notes[i].midi)
        continue;
      notes[i].endSeconds = std::max(notes[i].endSeconds, notes[j].endSeconds);
      notes[i].confidence = std::max(notes[i].confidence, notes[j].confidence);
      keep[j] = 0;
    }
  }

  // --- notes that are only somebody else's harmonic ---
  //
  // A note's overtones sit an octave, a twelfth and a double octave above it, loudly enough that a
  // transcriber reports them as notes of their own. Told apart from real ones by what they sit over:
  // a ghost is much quieter than its parent and lasts no longer than it does, whereas a real octave
  // is played at its own strength and starts and stops on its own.
  {
    // How much louder the note underneath has to be before this one is called its harmonic. Three
    // times at the loosest setting is ten decibels, which only a ghost is; at the strictest, any
    // note quieter than the one it sits over goes.
    const float louder = 3.0f - 1.8f * strength;
    for (size_t i = 0; i < notes.size(); i++)
    {
      if (!keep[i])
        continue;

      const double span = std::max(1.0e-6, notes[i].DurationSeconds());
      for (size_t j = firstThatCouldOverlap(i); j < notes.size(); j++)
      {
        if (j == i || !keep[j])
          continue;
        if (notes[j].startSeconds > notes[i].endSeconds)
          break;

        // Above its parent is a harmonic; an octave below one is the transcriber halving a pitch it
        // was unsure of. Both are heard as the same note twice.
        const int interval = notes[i].midi - notes[j].midi;
        if (interval != 12 && interval != 19 && interval != 24 && interval != -12)
          continue;
        if (notes[j].confidence < notes[i].confidence * louder)
          continue;

        const double overlap = std::min(notes[i].endSeconds, notes[j].endSeconds)
                               - std::max(notes[i].startSeconds, notes[j].startSeconds);
        if (overlap >= span * 0.6)
        {
          keep[i] = 0;
          break;
        }
      }
    }
  }

  // --- too quiet to have been played ---
  //
  // Measured against this track's own notes rather than against a fixed number, because a stem and
  // a full mix arrive at completely different levels and the question is the same in both: is this
  // one of the notes here, or is it a tail of one.
  {
    std::vector<float> levels;
    levels.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); i++)
      if (keep[i])
        levels.push_back(notes[i].confidence);

    const float median = MedianOf(std::move(levels));
    const float quietest = median * (0.10f + 0.55f * strength);
    for (size_t i = 0; i < notes.size(); i++)
      if (keep[i] && notes[i].confidence < quietest)
        keep[i] = 0;
  }

  // --- too short to have been played ---
  //
  // Thirty milliseconds at the loosest setting, which is under a demisemiquaver at any tempo anyone
  // plays; a hundred and seventy at the strictest, which is a note you would write down.
  const double shortest = 0.03 + 0.14 * static_cast<double>(strength);
  for (size_t i = 0; i < notes.size(); i++)
    if (keep[i] && notes[i].DurationSeconds() < shortest)
      keep[i] = 0;

  // --- more notes at once than the instrument has ---
  //
  // Walked left to right holding what is currently sounding. When a note starts into a full set,
  // the quietest of the set and the newcomer goes - whichever that is, so a loud note arriving can
  // displace a quiet one already running. What is left over is what the transcriber was least sure
  // of, which is where its inventions are.
  //
  // A limit of zero asks nothing, which is right for a detector that hears one note anyway.
  if (track.maxVoices > 0)
  {
    std::vector<size_t> sounding;
    sounding.reserve(static_cast<size_t>(track.maxVoices) + 1);

    for (size_t i = 0; i < notes.size(); i++)
    {
      if (!keep[i])
        continue;

      const double now = notes[i].startSeconds;
      sounding.erase(std::remove_if(sounding.begin(), sounding.end(),
                                    [&](size_t index) { return notes[index].endSeconds <= now; }),
                     sounding.end());

      sounding.push_back(i);
      if (static_cast<int>(sounding.size()) <= track.maxVoices)
        continue;

      size_t quietest = 0;
      for (size_t k = 1; k < sounding.size(); k++)
        if (notes[sounding[k]].confidence < notes[sounding[quietest]].confidence)
          quietest = k;

      keep[sounding[quietest]] = 0;
      sounding.erase(sounding.begin() + static_cast<std::ptrdiff_t>(quietest));
    }
  }

  track.notes.reserve(notes.size());
  for (size_t i = 0; i < notes.size(); i++)
    if (keep[i])
      track.notes.push_back(notes[i]);

  MeasureRange(track);
}

NoteAnalyser::~NoteAnalyser()
{
  if (mWorker.joinable())
    mWorker.join();
}

void NoteAnalyser::Start(std::shared_ptr<AudioFile> file, int trackId)
{
  if (mRunning.load(std::memory_order_relaxed) || !file || file->Empty())
    return;

  if (mWorker.joinable())
    mWorker.join();

  mTrackId.store(trackId, std::memory_order_relaxed);
  mRunning.store(true, std::memory_order_relaxed);
  mDone.store(false, std::memory_order_relaxed);

  mWorker = std::thread(
    [this, file]()
    {
      mResult = Analyse(*file);
      mDone.store(true, std::memory_order_release);
      mRunning.store(false, std::memory_order_relaxed);
    });
}

bool NoteAnalyser::Consume(int& trackId, NoteTrack& out)
{
  if (!mDone.load(std::memory_order_acquire))
    return false;

  if (mWorker.joinable())
    mWorker.join();
  mDone.store(false, std::memory_order_relaxed);

  trackId = mTrackId.load(std::memory_order_relaxed);
  out = std::move(mResult);
  mResult = NoteTrack();
  return true;
}

NoteTrack NoteAnalyser::Analyse(const AudioFile& file)
{
  NoteTrack result;

  double rate = 0.0;
  const std::vector<float> signal = Decimate(file, rate);
  if (signal.size() < kWindow + kTauMax + 1 || rate <= 0.0)
    return result;

  const size_t lastStart = signal.size() - kWindow - kTauMax - 1;
  const size_t frameCount = lastStart / kHop + 1;

  std::vector<Frame> frames(frameCount);

  // --- first pass: how loud each frame is ---
  //
  // Cheap, and it decides which frames are worth the expensive part. A stem is mostly silence
  // between phrases, and silence has no pitch to find.
  float loudest = 0.0f;
  for (size_t f = 0; f < frameCount; f++)
  {
    const size_t start = f * kHop;
    double sum = 0.0;
    for (size_t j = 0; j < kWindow; j++)
      sum += static_cast<double>(signal[start + j]) * static_cast<double>(signal[start + j]);
    frames[f].rms = static_cast<float>(std::sqrt(sum / static_cast<double>(kWindow)));
    loudest = std::max(loudest, frames[f].rms);
  }

  // Thirty dB below the loudest moment. Below that a stem is bleed from the other stems, and
  // pitching it produces notes nobody played.
  const float floorRms = std::max(1.0e-4f, loudest * 0.03f);

  // --- second pass: the pitch of the frames that have one ---
  std::vector<double> difference(kTauMax + 2, 0.0);
  std::vector<double> normalised(kTauMax + 2, 0.0);

  for (size_t f = 0; f < frameCount; f++)
  {
    if (frames[f].rms < floorRms)
      continue;

    double confidence = 0.0;
    const double hz = EstimatePitch(signal, f * kHop, rate, difference, normalised, confidence);
    if (hz <= 0.0 || confidence < kMinConfidence)
      continue;

    frames[f].midi = static_cast<float>(69.0 + 12.0 * std::log2(hz / 440.0));
    frames[f].confidence = static_cast<float>(confidence);
    frames[f].voiced = true;
  }

  // --- a light median filter over the pitch ---
  //
  // Five frames is 80 ms. Shorter than any note, longer than the single-frame octave jumps that
  // survive the threshold, so it removes those and nothing else.
  if (frameCount >= 5)
  {
    std::vector<float> filtered(frameCount);
    for (size_t f = 0; f < frameCount; f++)
    {
      filtered[f] = frames[f].midi;
      if (!frames[f].voiced)
        continue;

      std::vector<float> window;
      for (size_t k = (f >= 2 ? f - 2 : 0); k <= std::min(frameCount - 1, f + 2); k++)
        if (frames[k].voiced)
          window.push_back(frames[k].midi);
      if (window.size() >= 3)
        filtered[f] = MedianOf(std::move(window));
    }
    for (size_t f = 0; f < frameCount; f++)
      frames[f].midi = filtered[f];
  }

  // --- frames into notes ---
  const double frameSeconds = static_cast<double>(kHop) / rate;
  const double windowSeconds = static_cast<double>(kWindow) / rate;

  std::vector<float> currentPitches;
  std::vector<float> currentConfidences;
  size_t noteStartFrame = 0;
  size_t lastVoicedFrame = 0;
  int gap = 0;
  bool inNote = false;

  const auto closeNote = [&]()
  {
    if (!inNote || currentPitches.empty())
    {
      inNote = false;
      currentPitches.clear();
      currentConfidences.clear();
      return;
    }

    DetectedNote note;
    // A frame's estimate describes the middle of its window, not its start: a window that is only
    // a quarter note still reads as that note, so taking the window edges puts every note half a
    // window early at one end and half a window late at the other. Measured centre to centre, plus
    // the hop the last frame stands for, notes land within about twenty milliseconds.
    note.startSeconds = static_cast<double>(noteStartFrame) * frameSeconds + windowSeconds * 0.5;
    note.endSeconds = static_cast<double>(lastVoicedFrame) * frameSeconds + windowSeconds * 0.5 + frameSeconds;

    const float median = MedianOf(currentPitches);
    note.midi = static_cast<int>(std::lround(median));
    note.centsOffset = std::clamp((median - static_cast<float>(note.midi)) * 100.0f, -50.0f, 50.0f);

    float total = 0.0f;
    for (const float c : currentConfidences)
      total += c;
    note.confidence = currentConfidences.empty() ? 0.0f : total / static_cast<float>(currentConfidences.size());

    if (note.DurationSeconds() >= kMinNoteSeconds)
    {
      // The same note either side of a short gap is one note that was picked twice, or one whose
      // middle went quiet. Either way it is not two notes to read.
      if (!result.notes.empty() && result.notes.back().midi == note.midi
          && note.startSeconds - result.notes.back().endSeconds < 0.04)
        result.notes.back().endSeconds = note.endSeconds;
      else
        result.notes.push_back(note);
    }

    inNote = false;
    currentPitches.clear();
    currentConfidences.clear();
  };

  for (size_t f = 0; f < frameCount; f++)
  {
    if (!frames[f].voiced)
    {
      if (inNote && ++gap > kMaxGapFrames)
        closeNote();
      continue;
    }

    if (inNote)
    {
      // Compared against where the note has been lately rather than against its first frame, so a
      // slide stays one note until it settles somewhere else.
      std::vector<float> recent(currentPitches.end() - static_cast<std::ptrdiff_t>(std::min<size_t>(5, currentPitches.size())),
                                currentPitches.end());
      if (std::fabs(frames[f].midi - MedianOf(std::move(recent))) > kNewNoteSemitones)
        closeNote();
    }

    if (!inNote)
    {
      inNote = true;
      noteStartFrame = f;
    }

    gap = 0;
    lastVoicedFrame = f;
    currentPitches.push_back(frames[f].midi);
    currentConfidences.push_back(frames[f].confidence);
  }
  closeNote();

  // --- notes whose fundamental is not actually there ---
  //
  // Several notes at once are periodic at the frequency they are all harmonics of, and that
  // frequency is usually an octave or two below anything anyone played. The detector is not wrong
  // - the signal really does repeat there - but the answer is a note that was never sounded, and
  // it comes back with high confidence, which is the worst combination.
  //
  // Told apart by looking: if there is no energy at the pitch reported but plenty an octave up,
  // the fundamental is missing and the octave up is what was played. A real low note has its own
  // fundamental, so this leaves those alone.
  for (auto& note : result.notes)
  {
    const size_t from = std::min(signal.size(), static_cast<size_t>(note.startSeconds * rate));
    const size_t to = std::min(signal.size(), static_cast<size_t>(note.endSeconds * rate));
    if (to - from < kWindow)
      continue;

    for (int attempt = 0; attempt < 2; attempt++)
    {
      const double hz = MidiToHz(static_cast<double>(note.midi) + note.centsOffset / 100.0);
      const double atFundamental = ToneMagnitude(signal, from, to, hz, rate);
      const double atOctave = ToneMagnitude(signal, from, to, hz * 2.0, rate);

      // Fourteen dB down is well past what a weak fundamental looks like and firmly into one that
      // is not present at all.
      if (atFundamental > atOctave * 0.2)
        break;

      note.midi += 12;
      // Doubling a frequency does not change where it sits within its semitone, so the cents stay
      // as they were. The confidence does not: this note was inferred, not heard.
      note.confidence *= 0.7f;
    }
  }

  // One note at a time by construction, so there is no second voice to cap. The cleanup still has
  // the fragments and the quiet tails to take, and it is the same control in the same place.
  result.rawNotes = std::move(result.notes);
  result.maxVoices = 0;
  result.cleanupStrength = 0.35f;
  ApplyNoteCleanup(result);

  result.valid = true;
  return result;
}

std::string MidiNoteName(int midi)
{
  const int index = ((midi % 12) + 12) % 12;
  const int octave = midi / 12 - 1; // MIDI 60 is C4
  return NoteName(index, octave);
}

// --- chords ---------------------------------------------------------------------------------

namespace
{

/// For chroma the question is which pitch classes are sounding, not what the waveform is doing, so
/// the signal can be band-limited harder than the pitch detector needs. 12 kHz keeps five octaves
/// of fundamentals and their first few harmonics.
constexpr double kChromaRate = 12000.0;
/// 4096 at 12 kHz is a 341 ms window, whose bins are 2.9 Hz apart. A semitone at the bottom of a
/// bass guitar is under 4 Hz, so this is about as short as a window can be and still tell two low
/// notes apart.
constexpr size_t kChromaWindow = 4096;
constexpr size_t kChromaHop = 1024;
/// The range folded into the twelve classes, and the narrower range the bass is read from.
constexpr int kLowestNote = 28;  ///< E1
constexpr int kHighestNote = 96; ///< C7
constexpr int kBassLowest = 28;
constexpr int kBassHighest = 52; ///< E3
/// Below this the best template is not convincing enough to name, and a gap is the honest answer.
constexpr float kMinChordScore = 0.62f;

struct ChordTemplate
{
  ChordQuality quality;
  int intervals[5];
  int count;
};

/// Every shape, as intervals from the root.
const ChordTemplate kTemplates[] = {
  {ChordQuality::Major, {0, 4, 7}, 3},
  {ChordQuality::Minor, {0, 3, 7}, 3},
  {ChordQuality::Dominant7, {0, 4, 7, 10}, 4},
  {ChordQuality::Minor7, {0, 3, 7, 10}, 4},
  {ChordQuality::Major7, {0, 4, 7, 11}, 4},
  {ChordQuality::MinorSeventhFlatFive, {0, 3, 6, 10}, 4},
  {ChordQuality::Diminished, {0, 3, 6}, 3},
  {ChordQuality::Augmented, {0, 4, 8}, 3},
  {ChordQuality::Sus2, {0, 2, 7}, 3},
  {ChordQuality::Sus4, {0, 5, 7}, 3},
  {ChordQuality::Fifth, {0, 7}, 2},
};

double NoteHz(double midi)
{
  return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

/// Magnitude at one frequency, interpolated between bins. At the bottom of the range a semitone is
/// barely wider than a bin, so taking the nearest one throws away the difference between a note
/// and its neighbour.
float MagnitudeAt(const std::vector<float>& magnitudes, double hz, double rate, size_t fftSize)
{
  const double bin = hz * static_cast<double>(fftSize) / rate;
  if (bin < 1.0 || bin >= static_cast<double>(magnitudes.size() - 1))
    return 0.0f;

  const size_t low = static_cast<size_t>(bin);
  const float fraction = static_cast<float>(bin - static_cast<double>(low));
  return magnitudes[low] * (1.0f - fraction) + magnitudes[low + 1] * fraction;
}

/// One frame's twelve pitch classes, and the same read from the bass alone.
struct Chroma
{
  std::array<float, 12> pitch{};
  std::array<float, 12> bass{};
  float energy = 0.0f;
};

void NormaliseInPlace(std::array<float, 12>& values)
{
  float total = 0.0f;
  for (const float v : values)
    total += v * v;
  total = std::sqrt(total);
  if (total <= 1.0e-9f)
    return;
  for (float& v : values)
    v /= total;
}

/// Cosine similarity between a chroma and a chord shape, with the bass given a say.
///
/// The chroma alone cannot tell C6 from Am7 - they are the same four notes - and it cannot tell a
/// chord from its own inversion. What is in the bass can, and is how a person tells them apart too.
float ScoreTemplate(const Chroma& chroma, int root, const ChordTemplate& shape)
{
  float dot = 0.0f;
  float inside = 0.0f;
  for (int i = 0; i < shape.count; i++)
  {
    const float value = chroma.pitch[static_cast<size_t>((root + shape.intervals[i]) % 12)];
    dot += value;
    inside += value * value;
  }

  float score = dot / std::sqrt(static_cast<float>(shape.count));

  // What the shape does not account for counts against it. Without this a template with fewer
  // notes always has the advantage - it is easier to match two notes well than four - and every
  // chord collapses to the two notes of it that happen to be loudest.
  //
  // The chroma is a unit vector, so anything not inside the shape is outside it.
  const float outside = std::sqrt(std::max(0.0f, 1.0f - inside));
  score -= 0.5f * outside;

  // A power chord is a claim that there is no third, not merely that the root and fifth are loud.
  // A guitar voicing of an open major chord has the root and the fifth three times over and the
  // third once, so without this it reads as a fifth every time.
  if (shape.quality == ChordQuality::Fifth)
  {
    const float third = std::max(chroma.pitch[static_cast<size_t>((root + 3) % 12)],
                                 chroma.pitch[static_cast<size_t>((root + 4) % 12)]);
    score *= std::clamp(1.0f - 2.0f * third, 0.0f, 1.0f);
  }

  // A root that is also the lowest thing sounding is the usual case, and worth a nudge rather than
  // a rule - plenty of real music puts the third or the fifth in the bass.
  return score * (1.0f + 0.25f * chroma.bass[static_cast<size_t>(root)]);
}

} // namespace

std::string ChordName(const DetectedChord& chord)
{
  const char* suffix = "";
  switch (chord.quality)
  {
    case ChordQuality::Minor: suffix = "m"; break;
    case ChordQuality::Dominant7: suffix = "7"; break;
    case ChordQuality::Minor7: suffix = "m7"; break;
    case ChordQuality::Major7: suffix = "maj7"; break;
    case ChordQuality::MinorSeventhFlatFive: suffix = "m7b5"; break;
    case ChordQuality::Diminished: suffix = "dim"; break;
    case ChordQuality::Augmented: suffix = "aug"; break;
    case ChordQuality::Sus2: suffix = "sus2"; break;
    case ChordQuality::Sus4: suffix = "sus4"; break;
    case ChordQuality::Fifth: suffix = "5"; break;
    case ChordQuality::Major:
    default: suffix = ""; break;
  }

  return std::string(kNoteNames[((chord.root % 12) + 12) % 12]) + suffix;
}

std::vector<int> ChordIntervals(ChordQuality quality)
{
  for (const auto& shape : kTemplates)
    if (shape.quality == quality)
      return std::vector<int>(shape.intervals, shape.intervals + shape.count);
  return {0, 4, 7};
}

ChordAnalyser::~ChordAnalyser()
{
  if (mWorker.joinable())
    mWorker.join();
}

void ChordAnalyser::Start(std::shared_ptr<AudioFile> file, int trackId, std::vector<double> beatSeconds)
{
  if (mRunning.load(std::memory_order_relaxed) || !file || file->Empty())
    return;

  if (mWorker.joinable())
    mWorker.join();

  mTrackId.store(trackId, std::memory_order_relaxed);
  mRunning.store(true, std::memory_order_relaxed);
  mDone.store(false, std::memory_order_relaxed);

  mWorker = std::thread(
    [this, file, beatSeconds]()
    {
      mResult = Analyse(*file, beatSeconds);
      mDone.store(true, std::memory_order_release);
      mRunning.store(false, std::memory_order_relaxed);
    });
}

bool ChordAnalyser::Consume(int& trackId, std::vector<DetectedChord>& out)
{
  if (!mDone.load(std::memory_order_acquire))
    return false;

  if (mWorker.joinable())
    mWorker.join();
  mDone.store(false, std::memory_order_relaxed);

  trackId = mTrackId.load(std::memory_order_relaxed);
  out = std::move(mResult);
  mResult.clear();
  return true;
}

std::vector<DetectedChord> ChordAnalyser::Analyse(const AudioFile& file, const std::vector<double>& beatSeconds)
{
  std::vector<DetectedChord> chords;

  double rate = 0.0;
  const std::vector<float> signal = Decimate(file, rate, kChromaRate);
  if (signal.size() < kChromaWindow * 2 || rate <= 0.0)
    return chords;

  const size_t frameCount = (signal.size() - kChromaWindow) / kChromaHop + 1;

  // Hann, worked out once rather than per frame.
  std::vector<float> window(kChromaWindow);
  for (size_t i = 0; i < kChromaWindow; i++)
    window[i] = 0.5f * (1.0f - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kChromaWindow - 1)));

  std::vector<float> real(kChromaWindow);
  std::vector<float> imag(kChromaWindow);
  std::vector<float> magnitudes(kChromaWindow / 2);
  std::vector<Chroma> frames(frameCount);

  // Weights for a note's own harmonics. A note that is really there is held up by its overtones;
  // one that is only somebody else's third harmonic is not, which is what stops a chromagram
  // hearing a fifth above every root.
  static const int kHarmonicOffsets[] = {0, 12, 19, 24, 28};
  static const float kHarmonicWeights[] = {1.0f, 0.5f, 0.33f, 0.25f, 0.2f};

  std::vector<float> salience(static_cast<size_t>(kHighestNote - kLowestNote + 1));

  for (size_t f = 0; f < frameCount; f++)
  {
    const size_t start = f * kChromaHop;
    for (size_t i = 0; i < kChromaWindow; i++)
    {
      real[i] = signal[start + i] * window[i];
      imag[i] = 0.0f;
    }
    ForwardFft(real, imag);

    float energy = 0.0f;
    for (size_t bin = 0; bin < magnitudes.size(); bin++)
    {
      magnitudes[bin] = std::sqrt(real[bin] * real[bin] + imag[bin] * imag[bin]);
      energy += magnitudes[bin];
    }
    frames[f].energy = energy / static_cast<float>(magnitudes.size());

    // The spectrum read at musical pitches rather than at even frequencies, each note reinforced by
    // where its harmonics would be.
    for (int note = kLowestNote; note <= kHighestNote; note++)
    {
      float total = 0.0f;
      for (int h = 0; h < 5; h++)
      {
        const int at = note + kHarmonicOffsets[h];
        if (at > kHighestNote + 24)
          break;
        total += kHarmonicWeights[h] * MagnitudeAt(magnitudes, NoteHz(at), rate, kChromaWindow);
      }
      salience[static_cast<size_t>(note - kLowestNote)] = total;
    }

    for (int note = kLowestNote; note <= kHighestNote; note++)
    {
      const float value = salience[static_cast<size_t>(note - kLowestNote)];
      const size_t pc = static_cast<size_t>(((note % 12) + 12) % 12);
      if (note >= 36)
        frames[f].pitch[pc] += value;
      if (note >= kBassLowest && note <= kBassHighest)
        frames[f].bass[pc] += value;
    }

    NormaliseInPlace(frames[f].pitch);
    NormaliseInPlace(frames[f].bass);
  }

  // --- one decision per beat ---
  //
  // Chords change on beats. Averaging between them rather than on a clock of its own both settles
  // the chromagram and puts the answer where the bars are drawn.
  std::vector<double> boundaries;
  if (beatSeconds.size() >= 2)
  {
    boundaries = beatSeconds;
  }
  else
  {
    const double duration = static_cast<double>(signal.size()) / rate;
    for (double t = 0.0; t < duration; t += 0.5)
      boundaries.push_back(t);
    boundaries.push_back(duration);
  }

  const double frameSeconds = static_cast<double>(kChromaHop) / rate;

  float loudest = 0.0f;
  for (const auto& frame : frames)
    loudest = std::max(loudest, frame.energy);
  const float quiet = loudest * 0.04f;

  struct Segment
  {
    double from;
    double to;
    int root;
    ChordQuality quality;
    float score;
    bool named;
  };
  std::vector<Segment> segments;

  for (size_t b = 0; b + 1 < boundaries.size(); b++)
  {
    const double from = boundaries[b];
    const double to = boundaries[b + 1];
    if (to <= from)
      continue;

    const size_t firstFrame = static_cast<size_t>(std::max(0.0, from / frameSeconds));
    const size_t lastFrame = std::min(frameCount, static_cast<size_t>(to / frameSeconds));
    if (firstFrame >= lastFrame)
      continue;

    Chroma averaged;
    float energy = 0.0f;
    for (size_t f = firstFrame; f < lastFrame; f++)
    {
      for (size_t pc = 0; pc < 12; pc++)
      {
        averaged.pitch[pc] += frames[f].pitch[pc];
        averaged.bass[pc] += frames[f].bass[pc];
      }
      energy = std::max(energy, frames[f].energy);
    }
    NormaliseInPlace(averaged.pitch);
    NormaliseInPlace(averaged.bass);

    Segment segment{from, to, 0, ChordQuality::Major, 0.0f, false};

    if (energy > quiet)
    {
      for (int root = 0; root < 12; root++)
      {
        for (const auto& shape : kTemplates)
        {
          const float score = ScoreTemplate(averaged, root, shape);
          if (score > segment.score)
          {
            segment.score = score;
            segment.root = root;
            segment.quality = shape.quality;
          }
        }
      }
      segment.named = segment.score >= kMinChordScore;
    }

    segments.push_back(segment);
  }

  // A single beat disagreeing with the two around it is the chromagram twitching, not a chord.
  for (size_t i = 1; i + 1 < segments.size(); i++)
  {
    const bool neighboursAgree = segments[i - 1].named && segments[i + 1].named
                                 && segments[i - 1].root == segments[i + 1].root
                                 && segments[i - 1].quality == segments[i + 1].quality;
    const bool differs = !segments[i].named || segments[i].root != segments[i - 1].root
                         || segments[i].quality != segments[i - 1].quality;
    if (neighboursAgree && differs)
    {
      segments[i].root = segments[i - 1].root;
      segments[i].quality = segments[i - 1].quality;
      segments[i].named = true;
      segments[i].score = std::min(segments[i - 1].score, segments[i + 1].score);
    }
  }

  // Runs of the same chord are one chord, however many beats it lasted.
  for (const auto& segment : segments)
  {
    if (!segment.named)
      continue;

    if (!chords.empty() && chords.back().root == segment.root && chords.back().quality == segment.quality
        && std::fabs(chords.back().endSeconds - segment.from) < 0.05)
    {
      chords.back().endSeconds = segment.to;
      chords.back().confidence = std::max(chords.back().confidence, segment.score);
      continue;
    }

    DetectedChord chord;
    chord.startSeconds = segment.from;
    chord.endSeconds = segment.to;
    chord.root = segment.root;
    chord.quality = segment.quality;
    chord.confidence = std::clamp(segment.score, 0.0f, 1.0f);
    chords.push_back(chord);
  }

  return chords;
}

} // namespace nam_ui
