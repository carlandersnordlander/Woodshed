#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AudioFile.h"

namespace nam_ui
{

/// One note found in a track. Times are in seconds from the start of the file, so they survive the
/// file being reloaded at another rate - the same reason loops are stored that way.
struct DetectedNote
{
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  int midi = 69; ///< 69 = A4, 60 = C4
  /// How far the playing actually sat from that note, -50..50. Kept rather than rounded away,
  /// because "E2, twenty cents flat" is more use than "E2".
  float centsOffset = 0.0f;
  float confidence = 0.0f; ///< 0..1, how periodic the signal was where this note was found

  double DurationSeconds() const { return endSeconds - startSeconds; }
};

/// The chord shapes worth telling apart. Deliberately not every chord that has a name: two
/// templates that share their notes cannot be told apart by what is sounding, and adding them only
/// makes the ones that can be told apart wrong more often. C6 and Am7 are the same four notes.
enum class ChordQuality
{
  Major,
  Minor,
  Dominant7,
  Minor7,
  Major7,
  MinorSeventhFlatFive,
  Diminished,
  Augmented,
  Sus2,
  Sus4,
  Fifth ///< root and fifth, no third - a power chord, which is most of rock
};

/// One chord, over the stretch of time it was heard.
struct DetectedChord
{
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  int root = 0; ///< 0 = C
  ChordQuality quality = ChordQuality::Major;
  float confidence = 0.0f;

  double DurationSeconds() const { return endSeconds - startSeconds; }
};

/// "Em", "A7", "Cmaj7", "D5".
std::string ChordName(const DetectedChord& chord);

/// The chord's notes, as semitones above its root. For playing one back, which is the quickest way
/// to find out whether the name over the waveform is right.
std::vector<int> ChordIntervals(ChordQuality quality);

/// What one track's analysis produced.
struct NoteTrack
{
  /// What is drawn and played: what survived the cleanup below.
  std::vector<DetectedNote> notes;
  /// \brief Everything the detector handed back, before the cleanup.
  ///
  /// Kept because the right amount of cleaning is a property of the recording, not of the detector,
  /// and the only way to find it is to see the result. Transcribing again to try another setting
  /// costs minutes; re-running the cleanup costs nothing.
  std::vector<DetectedNote> rawNotes;
  /// Chords, which are found from the audio rather than from the notes above - so a track can have
  /// these without having those, and without needing anything installed.
  std::vector<DetectedChord> chords;
  /// The range the notes span, for laying them out vertically. Equal when there is one note.
  int lowestMidi = 60;
  int highestMidi = 60;
  bool valid = false;
  /// Whether this track's lane is showing them. Per track, because a drum stem's "notes" are
  /// nonsense you want out of the way while the bass stays up.
  bool visible = true;
  /// True when these came from the polyphonic transcriber, so notes overlap and the lane is drawn
  /// as a piano roll rather than as a single line.
  bool polyphonic = false;

  /// How hard the cleanup works, 0 to 1. Zero draws everything that was found.
  float cleanupStrength = 0.5f;
  /// \brief The most notes allowed to sound at once, or zero for no limit.
  ///
  /// Six is a guitar, four is most keyboard playing, one is a bass line or a voice. A transcriber
  /// asked for every note will happily report ten at once, and the ones past what the instrument
  /// has are the ones it was least sure about.
  int maxVoices = 0;

  /// Whether what was found here is played back, and on what. Off by default: an analysis you can
  /// see is useful on its own, and one that started making noise would be a surprise.
  bool playNotes = false;
  bool playChords = false;
  int instrument = 0; ///< a SynthInstrument
  float playGainDb = -6.0f;
  /// \brief Octaves to move the playback by, -3 to 3.
  ///
  /// A bass line played back on a piano at written pitch sits in the same register as the bass it
  /// was found in, and the two fight; up two octaves it sits above the music and can be heard
  /// against it. Only the playback moves - what was found, and what the lane draws, is unchanged.
  int playOctave = 0;
};

/// \brief Rebuilds `notes` from `rawNotes`, throwing away what is not playing.
///
/// Every note detector errs on the side of reporting. A transcriber that missed the quiet notes
/// would be useless, so it reports them, and it also reports the harmonics of loud notes, the
/// reverb after a phrase, and a scatter of twenty-millisecond fragments. All of that is real in the
/// signal and none of it is anybody playing.
///
/// What is thrown away, in order:
///   - the same note picked twice across a short gap, which is one note
///   - a note that is the octave, twelfth or double octave of a much louder note sounding under it
///   - notes far quieter than this track's own notes usually are
///   - notes too short to have been played
///   - past the first `maxVoices`, whatever is quietest at the moment it starts
///
/// Cheap enough to run while a slider moves, so the setting can be found by looking rather than by
/// guessing and transcribing again.
void ApplyNoteCleanup(NoteTrack& track);

/// \brief Finds the notes in a track, one note at a time.
///
/// YIN (de Cheveigné and Kawahara, 2002): the difference function of the signal against itself at
/// every lag, normalised so that a lag's score says how much *worse* it is than the best lag so
/// far. That normalisation is the whole trick - it is what stops the detector reporting an octave
/// down, which a plain autocorrelation does constantly.
///
/// Monophonic by construction. A bass or a vocal stem is close enough to one note at a time for
/// this to be right nearly always; a strummed chord is not, and no amount of tuning this makes it
/// so. That case wants a trained model, and is deliberately not attempted here.
class NoteAnalyser
{
public:
  ~NoteAnalyser();

  NoteAnalyser(const NoteAnalyser&) = delete;
  NoteAnalyser& operator=(const NoteAnalyser&) = delete;
  NoteAnalyser() = default;

  /// Starts analysing on a worker thread. Does nothing while one is already running. The file is
  /// held by shared_ptr for the duration, so closing the track mid-analysis is safe.
  void Start(std::shared_ptr<AudioFile> file, int trackId);
  bool IsRunning() const { return mRunning.load(std::memory_order_relaxed); }
  /// Which track is being worked on, or 0.
  int RunningTrackId() const { return mRunning.load(std::memory_order_relaxed) ? mTrackId.load(std::memory_order_relaxed) : 0; }

  /// Joins a finished worker and hands back what it found. False while it is still working, or
  /// when there is nothing new.
  bool Consume(int& trackId, NoteTrack& out);

private:
  static NoteTrack Analyse(const AudioFile& file);

  std::thread mWorker;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mDone{false};
  std::atomic<int> mTrackId{0};
  NoteTrack mResult;
};

/// \brief Works out what chords are being played, straight from the audio.
///
/// Nothing to do with the note detector above, and deliberately so. Naming a chord does not need
/// every note in it identified - it needs to know which of the twelve pitch classes are sounding
/// and how strongly, which is a far easier question and a far more robust one. A chromagram gives
/// that, and the answer is the template it looks most like.
///
/// Because it is only counting energy per pitch class, it works on a full mix as happily as on a
/// stem, it runs here in about a second, and it needs nothing installed.
class ChordAnalyser
{
public:
  ~ChordAnalyser();

  ChordAnalyser(const ChordAnalyser&) = delete;
  ChordAnalyser& operator=(const ChordAnalyser&) = delete;
  ChordAnalyser() = default;

  /// \param beatSeconds where the beats fall, from the song's grid. Chords change on beats, so
  ///        measuring between them rather than on a clock of its own is both cleaner and lines the
  ///        answer up with the bars drawn above it. Empty falls back to a fixed window.
  void Start(std::shared_ptr<AudioFile> file, int trackId, std::vector<double> beatSeconds);

  bool IsRunning() const { return mRunning.load(std::memory_order_relaxed); }
  int RunningTrackId() const { return mRunning.load(std::memory_order_relaxed) ? mTrackId.load(std::memory_order_relaxed) : 0; }

  bool Consume(int& trackId, std::vector<DetectedChord>& out);

private:
  static std::vector<DetectedChord> Analyse(const AudioFile& file, const std::vector<double>& beatSeconds);

  std::thread mWorker;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mDone{false};
  std::atomic<int> mTrackId{0};
  std::vector<DetectedChord> mResult;
};

/// "E2", "C#4". Sharps rather than flats, because a guitarist reading a fretboard thinks in them.
std::string MidiNoteName(int midi);

} // namespace nam_ui
