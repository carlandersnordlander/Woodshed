#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "AudioFile.h"

namespace nam_ui
{

/// What the analysis found. `confidence` is how far the winning tempo stood out from the rest,
/// roughly 0..1 - worth showing, because a low number means "check this before you trust it".
///
/// \brief The grid, as a list of where the beats actually are rather than as one number.
///
/// A single tempo cannot describe a real recording. Players drift, tape drifts, and even a song cut
/// to a click drifts against a grid drawn from an estimate: being 0.1% out is a tenth of a beat
/// after two minutes and a whole beat after twenty. The error accumulates because the grid is
/// extrapolated, and nothing about a single number can stop it.
///
/// So the beats are tracked through the song and stored. `bpm` survives as the median, for showing
/// and for the metronome, and everything that draws or snaps goes through the accessors below - so
/// a followed grid and a typed-in one behave identically everywhere.
struct TempoEstimate
{
  float bpm = 120.0f;
  double firstBeatSeconds = 0.0;
  float confidence = 0.0f;
  bool valid = false;

  /// Every beat found, in seconds, ascending. Empty when the tempo was typed in rather than found,
  /// in which case the accessors fall back to `bpm` and `firstBeatSeconds`.
  std::vector<double> beats;
  /// Which beat is a downbeat: `beats[downbeatOffset]`, and every bar length after it.
  int downbeatOffset = 0;

  /// \brief Which beats start a bar, one entry per beat, when that is known for each of them.
  ///
  /// A single offset plus a bar length can only describe music that keeps one time signature from
  /// the first beat to the last. A pickup, a bar of 2/4 dropped into a chorus, or a song that
  /// changes meter halfway all break it - and drawing bar lines from the offset then puts them on
  /// the wrong beats for the rest of the song.
  ///
  /// Filled by the model, which is asked this directly. Empty when the beats came from the signal
  /// analysis, which has only the offset to go on.
  std::vector<unsigned char> downbeats;
  /// The range the tempo moved over, for saying how much a song drifts without being asked.
  float lowestBpm = 0.0f;
  float highestBpm = 0.0f;

  /// Where beat `index` falls. Indices outside the map are extrapolated at the tempo at that end,
  /// so callers can walk indices without checking bounds.
  double BeatTime(long long index) const;
  /// The beat at or before `seconds`. May be negative, before the first one.
  long long BeatIndexAt(double seconds) const;
  /// The nearest beat to `seconds`, for snapping.
  double NearestBeat(double seconds) const;
  /// How long the beat at `index` lasts. The local tempo, in other words.
  double BeatLength(long long index) const;

  /// True when the song moves around enough to be worth mentioning.
  bool Drifts() const { return highestBpm > 0.0f && (highestBpm - lowestBpm) > 1.5f; }

  /// Moves every beat by `delta` seconds, for nudging the grid by hand without losing it.
  void Shift(double delta);
  /// Stretches the map about its first beat, for correcting the tempo by hand.
  void Scale(double factor);

  /// \brief Counts the same beats at half or double the rate, leaving every one where it is.
  ///
  /// Which pulse a song is "on" is a judgement rather than a measurement - a track with eighths
  /// running through it is as periodic at the eighths as at the quarters, and both readings are
  /// true. The analysis has to guess; these make correcting the guess exact and instant, without
  /// throwing away the timing it did get right.
  void Halve();
  void Double();
};

/// \brief Repairs beats and bars against what the ones either side of them say.
///
/// A tracker that is right almost everywhere still drops the occasional beat and marks the
/// occasional downbeat that is not one. Neither is a judgement call: a gap of twice the local beat
/// has a beat missing from the middle of it, a gap of half has one too many, and a bar of one beat
/// in a song of fours is a downbeat that should not be there. All three can be settled by looking
/// at the neighbours, which is what this does.
///
/// Local rather than global, so a song that changes tempo or meter on purpose is left alone: every
/// test is against the median of the beats around it, not against the piece.
void RegulariseBeats(TempoEstimate& estimate);

/// \brief Finds the tempo of a track, and where every one of its beats falls.
///
/// Spectral flux for the onsets, a comb filter over their autocorrelation for the tempo, and then
/// the beats themselves by dynamic programming: the sequence of beats that best balances landing
/// on the onsets against keeping a steady spacing. That trade-off is the whole point - a tracker
/// that only followed the onsets would jump at every syncopation, and one that only kept time
/// would walk away from the song.
///
/// Beat tracking is wrong on real music often enough that everything it produces is a starting
/// point rather than an answer. The UI has to let both numbers be corrected by hand, and this is
/// why.
class TempoAnalyser
{
public:
  ~TempoAnalyser();

  /// Starts analysing on a worker thread. Does nothing while one is already running.
  void Start(std::shared_ptr<AudioFile> file);
  bool IsRunning() const { return mRunning.load(std::memory_order_relaxed); }

  /// Joins a finished worker and hands back what it found. False while it is still working, or
  /// when there is nothing new.
  bool Consume(TempoEstimate& out);

private:
  static TempoEstimate Analyse(const AudioFile& file);

  std::thread mWorker;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mDone{false};
  TempoEstimate mResult;
};

} // namespace nam_ui
