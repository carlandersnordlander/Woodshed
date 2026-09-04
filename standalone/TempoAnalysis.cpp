#include "TempoAnalysis.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "ParametricEq.h" // ForwardFft

namespace nam_ui
{

namespace
{
constexpr size_t kWindow = 1024;
constexpr size_t kHop = 512;

/// The range worth searching. Below 60 and above 200 you are almost always looking at half or
/// double of something inside it.
constexpr float kMinBpm = 60.0f;
constexpr float kMaxBpm = 200.0f;
constexpr float kBpmStep = 0.25f;

/// Tempos are searched on a log scale weighted towards this, which is how the half-and-double
/// ambiguity gets broken. A machine cannot tell 80 from 160 by periodicity alone; people hear the
/// one nearer a walking pace.
constexpr double kPreferredBpm = 120.0;
constexpr double kPreferenceWidth = 0.9;

double TempoPreference(double bpm)
{
  const double octaves = std::log2(bpm / kPreferredBpm) / kPreferenceWidth;
  return std::exp(-0.5 * octaves * octaves);
}

/// How hard the tracker is held to the estimated tempo. Low and it chases every loud syncopation;
/// high and it ignores the song and keeps its own time, which is the thing being fixed here.
constexpr double kTightness = 100.0;

/// \brief The beats themselves, by dynamic programming over the onset envelope.
///
/// For every frame, the best possible score of a beat sequence ending there:
///
///     score[n] = onset[n] + max over gaps d of ( score[n-d] - tightness * log(d/period)^2 )
///
/// The penalty is on the *ratio* of the gap to the expected period, so it is symmetric in tempo -
/// six percent fast costs what six percent slow does. Following the backpointers from the best
/// ending gives the beats. Because each step only prefers the period rather than requiring it, the
/// result bends with a song that speeds up and holds steady where nothing is playing.
///
/// After Ellis, "Beat Tracking by Dynamic Programming" (2007).
std::vector<size_t> TrackBeats(const std::vector<float>& onset, double periodFrames)
{
  std::vector<size_t> beats;
  const size_t count = onset.size();
  if (count < 4 || periodFrames < 2.0)
    return beats;

  // The penalty constant only means anything against an envelope of known scale.
  double sum = 0.0;
  for (const float value : onset)
    sum += static_cast<double>(value);
  const double mean = sum / static_cast<double>(count);

  double variance = 0.0;
  for (const float value : onset)
    variance += (value - mean) * (value - mean);
  const double deviation = std::sqrt(variance / static_cast<double>(count));
  if (deviation <= 1.0e-9)
    return beats;

  std::vector<double> strength(count);
  for (size_t i = 0; i < count; i++)
    strength[i] = (static_cast<double>(onset[i]) - mean) / deviation;

  const long long minGap = std::max<long long>(1, static_cast<long long>(std::round(periodFrames * 0.5)));
  const long long maxGap = static_cast<long long>(std::round(periodFrames * 2.0));

  std::vector<double> score(count, 0.0);
  std::vector<long long> previous(count, -1);

  // The penalty for each allowed gap, worked out once rather than per frame.
  std::vector<double> penalty(static_cast<size_t>(maxGap - minGap + 1));
  for (long long gap = minGap; gap <= maxGap; gap++)
  {
    const double ratio = std::log(static_cast<double>(gap) / periodFrames);
    penalty[static_cast<size_t>(gap - minGap)] = -kTightness * ratio * ratio;
  }

  for (size_t n = 0; n < count; n++)
  {
    double best = 0.0;
    long long bestFrom = -1;

    const long long lowest = static_cast<long long>(n) - maxGap;
    const long long highest = static_cast<long long>(n) - minGap;
    for (long long from = std::max<long long>(0, lowest); from <= highest; from++)
    {
      const long long gap = static_cast<long long>(n) - from;
      const double candidate = score[static_cast<size_t>(from)] + penalty[static_cast<size_t>(gap - minGap)];
      if (bestFrom < 0 || candidate > best)
      {
        best = candidate;
        bestFrom = from;
      }
    }

    score[n] = strength[n] + (bestFrom >= 0 ? best : 0.0);
    previous[n] = bestFrom;
  }

  // The score only grows, so the best sequence ends near the end of the song.
  size_t last = 0;
  for (size_t n = 0; n < count; n++)
    if (score[n] > score[last])
      last = n;

  for (long long at = static_cast<long long>(last); at >= 0; at = previous[static_cast<size_t>(at)])
  {
    beats.push_back(static_cast<size_t>(at));
    if (previous[static_cast<size_t>(at)] < 0)
      break;
  }
  std::reverse(beats.begin(), beats.end());
  return beats;
}

} // namespace

double TempoEstimate::BeatLength(long long index) const
{
  if (beats.size() < 2)
    return bpm > 0.0f ? 60.0 / static_cast<double>(bpm) : 0.5;

  // Clamped to the map, so beats either side of the song keep the tempo it had at that end.
  const long long last = static_cast<long long>(beats.size()) - 2;
  const long long at = std::clamp(index, 0LL, last);
  return beats[static_cast<size_t>(at) + 1] - beats[static_cast<size_t>(at)];
}

double TempoEstimate::BeatTime(long long index) const
{
  if (beats.empty())
    return firstBeatSeconds + static_cast<double>(index) * (bpm > 0.0f ? 60.0 / static_cast<double>(bpm) : 0.5);

  if (index < 0)
    return beats.front() + static_cast<double>(index) * BeatLength(0);

  const long long size = static_cast<long long>(beats.size());
  if (index >= size)
    return beats.back() + static_cast<double>(index - (size - 1)) * BeatLength(size - 2);

  return beats[static_cast<size_t>(index)];
}

long long TempoEstimate::BeatIndexAt(double seconds) const
{
  if (beats.empty())
  {
    const double length = bpm > 0.0f ? 60.0 / static_cast<double>(bpm) : 0.5;
    return static_cast<long long>(std::floor((seconds - firstBeatSeconds) / length));
  }

  if (seconds < beats.front())
    return static_cast<long long>(std::floor((seconds - beats.front()) / BeatLength(0)));

  if (seconds >= beats.back())
  {
    const long long size = static_cast<long long>(beats.size());
    return (size - 1) + static_cast<long long>(std::floor((seconds - beats.back()) / BeatLength(size - 2)));
  }

  const auto at = std::upper_bound(beats.begin(), beats.end(), seconds);
  return static_cast<long long>(std::distance(beats.begin(), at)) - 1;
}

double TempoEstimate::NearestBeat(double seconds) const
{
  const long long index = BeatIndexAt(seconds);
  const double before = BeatTime(index);
  const double after = BeatTime(index + 1);
  return (seconds - before <= after - seconds) ? before : after;
}

void TempoEstimate::Shift(double delta)
{
  firstBeatSeconds += delta;
  for (double& beat : beats)
    beat += delta;
}

void TempoEstimate::Scale(double factor)
{
  if (beats.size() < 2 || factor <= 0.0)
    return;
  const double anchor = beats.front();
  for (double& beat : beats)
    beat = anchor + (beat - anchor) * factor;
}

void TempoEstimate::Halve()
{
  bpm *= 0.5f;
  lowestBpm *= 0.5f;
  highestBpm *= 0.5f;

  if (beats.size() < 4)
    return;

  // Every other beat, kept where it already is. The song's own timing survives exactly - this only
  // changes which level is being counted, which is the one thing the analysis could not know.
  std::vector<double> halved;
  halved.reserve(beats.size() / 2 + 1);
  for (size_t i = downbeatOffset % 2; i < beats.size(); i += 2)
    halved.push_back(beats[i]);

  beats = std::move(halved);
  downbeatOffset /= 2;
  firstBeatSeconds = beats.front();
}

void TempoEstimate::Double()
{
  bpm *= 2.0f;
  lowestBpm *= 2.0f;
  highestBpm *= 2.0f;

  if (beats.size() < 2)
    return;

  // A beat between each pair, and one past the end so the last beat still has a partner. Halfway
  // is right by construction: the two beats either side were tracked, so the midpoint drifts with
  // them rather than against them.
  std::vector<double> doubled;
  doubled.reserve(beats.size() * 2);
  for (size_t i = 0; i + 1 < beats.size(); i++)
  {
    doubled.push_back(beats[i]);
    doubled.push_back(0.5 * (beats[i] + beats[i + 1]));
  }
  doubled.push_back(beats.back());
  doubled.push_back(beats.back() + BeatLength(static_cast<long long>(beats.size()) - 2) * 0.5);

  beats = std::move(doubled);
  downbeatOffset *= 2;
  firstBeatSeconds = beats.front();
}

void RegulariseBeats(TempoEstimate& estimate)
{
  if (estimate.beats.size() < 8)
    return;

  const bool hasFlags = estimate.downbeats.size() == estimate.beats.size();

  // --- the beats themselves ---
  //
  // What the beat is worth around here, as the median of a wide window. Wide on purpose: a drum
  // fill is a run of beats far too close together, and over four either side the fill *is* the
  // neighbourhood, so it looks perfectly normal to itself. Over sixteen either side it cannot
  // outvote the music around it - a median is unmoved until half its input is wrong - while a song
  // that genuinely speeds up is still followed, because the window travels with it.
  const auto robustLength = [&](size_t at)
  {
    constexpr size_t kSpan = 16;
    const size_t from = (at > kSpan) ? at - kSpan : 1;
    const size_t to = std::min(estimate.beats.size(), at + kSpan + 1);

    std::vector<double> gaps;
    gaps.reserve(to - from);
    for (size_t i = from; i < to; i++)
      gaps.push_back(estimate.beats[i] - estimate.beats[i - 1]);
    if (gaps.empty())
      return 0.0;

    std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
    return gaps[gaps.size() / 2];
  };

  const auto agrees = [&](size_t gapIndex)
  {
    const double length = robustLength(gapIndex);
    if (length <= 0.0)
      return true;
    const double gap = estimate.beats[gapIndex] - estimate.beats[gapIndex - 1];
    return gap > length * 0.72 && gap < length * 1.42;
  };

  std::vector<double> beats;
  std::vector<unsigned char> downbeats;
  beats.reserve(estimate.beats.size() + 8);
  downbeats.reserve(estimate.beats.size() + 8);

  beats.push_back(estimate.beats.front());
  downbeats.push_back(hasFlags ? estimate.downbeats.front() : 0);

  size_t i = 1;
  while (i < estimate.beats.size())
  {
    if (agrees(i))
    {
      beats.push_back(estimate.beats[i]);
      downbeats.push_back(hasFlags ? estimate.downbeats[i] : 0);
      i++;
      continue;
    }

    // A stretch that does not hold the beat. Extended to its end, so a fill is dealt with as one
    // passage rather than one gap at a time - fixing it gap by gap is what leaves the click
    // chasing every hit.
    size_t end = i;
    while (end < estimate.beats.size() && !agrees(end))
      end++;

    // Laid out evenly between the last beat that was right and the next one that is, as many as
    // fit the beat around here. That is the whole idea: the beats either side are trustworthy, so
    // the passage between them is a straight line drawn from one to the other.
    const double from = beats.back();
    const double to = (end < estimate.beats.size()) ? estimate.beats[end] : estimate.beats.back();
    const double length = robustLength(i);
    const double span = to - from;

    const long long expected =
      (length > 0.0) ? std::max<long long>(1, std::lround(span / length)) : 1;

    for (long long step = 1; step <= expected; step++)
    {
      const double at = from + span * static_cast<double>(step) / static_cast<double>(expected);

      // The downbeat nearest each new beat comes with it, so a bar line inside the passage lands
      // on the beat that replaced the one carrying it rather than being lost.
      unsigned char flag = 0;
      if (hasFlags)
      {
        double nearest = 1.0e9;
        for (size_t j = i; j <= end && j < estimate.beats.size(); j++)
        {
          if (estimate.downbeats[j] == 0)
            continue;
          const double distance = std::fabs(estimate.beats[j] - at);
          if (distance < nearest && distance < length * 0.5)
          {
            nearest = distance;
            flag = 1;
          }
        }
      }

      beats.push_back(at);
      downbeats.push_back(flag);
    }

    i = end + 1;
  }

  estimate.beats = std::move(beats);
  if (hasFlags)
    estimate.downbeats = std::move(downbeats);
  estimate.firstBeatSeconds = estimate.beats.front();

  if (!hasFlags || estimate.downbeats.size() != estimate.beats.size())
    return;

  // --- the bars ---
  //
  // The same argument one level up. A bar of one beat in a song of fours is a downbeat that should
  // not be there; a bar of seven is one that is missing. The commonest length is what the song is
  // in, and anything at odds with it is repaired towards it - but a bar of three among fours is
  // left alone, because that is a thing music does.
  const auto collectBars = [&]()
  {
    std::vector<size_t> bars;
    for (size_t i = 0; i < estimate.downbeats.size(); i++)
      if (estimate.downbeats[i] != 0)
        bars.push_back(i);
    return bars;
  };

  std::vector<size_t> bars = collectBars();
  if (bars.size() < 4)
    return;

  std::array<int, 17> votes{};
  for (size_t i = 1; i < bars.size(); i++)
  {
    const size_t length = bars[i] - bars[i - 1];
    if (length >= 1 && length < votes.size())
      votes[length]++;
  }

  size_t modal = 4;
  for (size_t length = 2; length < votes.size(); length++)
    if (votes[length] > votes[modal])
      modal = length;

  // Two passes, because removing a downbeat changes where the bar after it begins - and joining
  // the two halves of what was really one bar is exactly what makes it long enough to be seen as
  // two. Done in one pass, the second half was still being measured from the boundary that had
  // just been taken away.
  for (size_t i = 1; i < bars.size(); i++)
    if ((bars[i] - bars[i - 1]) * 2 <= modal)
      estimate.downbeats[bars[i]] = 0; // too short to be a bar of anything

  bars = collectBars();
  for (size_t i = 1; i < bars.size(); i++)
  {
    const size_t length = bars[i] - bars[i - 1];

    // Long enough to be two bars with the join missed. Only when it divides cleanly - a bar of
    // seven among fours is left as it is rather than being cut into four and three by guesswork.
    if (length >= modal * 2 && (length % modal) == 0)
      for (size_t at = bars[i - 1] + modal; at < bars[i]; at += modal)
        estimate.downbeats[at] = 1;
  }

  for (size_t i = 0; i < estimate.downbeats.size(); i++)
  {
    if (estimate.downbeats[i] == 0)
      continue;
    estimate.downbeatOffset = static_cast<int>(i);
    break;
  }
}

TempoAnalyser::~TempoAnalyser()
{
  if (mWorker.joinable())
    mWorker.join();
}

void TempoAnalyser::Start(std::shared_ptr<AudioFile> file)
{
  if (mRunning.load(std::memory_order_relaxed) || !file || file->Empty())
    return;

  if (mWorker.joinable())
    mWorker.join();

  mRunning.store(true, std::memory_order_relaxed);
  mDone.store(false, std::memory_order_relaxed);

  // The file is held by shared_ptr for the duration, so the track can be closed while this runs
  // without the analysis reading freed samples.
  mWorker = std::thread(
    [this, file]()
    {
      mResult = Analyse(*file);
      mDone.store(true, std::memory_order_release);
      mRunning.store(false, std::memory_order_relaxed);
    });
}

bool TempoAnalyser::Consume(TempoEstimate& out)
{
  if (!mDone.load(std::memory_order_acquire))
    return false;

  if (mWorker.joinable())
    mWorker.join();
  mDone.store(false, std::memory_order_relaxed);
  out = mResult;
  return true;
}

TempoEstimate TempoAnalyser::Analyse(const AudioFile& file)
{
  TempoEstimate estimate;

  const size_t frames = file.FrameCount();
  if (frames < kWindow * 4)
    return estimate;

  const double frameRate = file.sampleRate / static_cast<double>(kHop);
  const size_t windows = (frames - kWindow) / kHop;
  if (windows < 32)
    return estimate;

  // --- onset envelope, from spectral flux ---
  //
  // Energy alone rises and falls with the music; what marks an onset is energy appearing in bins
  // that did not have it a moment ago. Only rises count - a note stopping is not an onset.
  //
  // Two things about *how* that is summed decide whether the envelope can tell a strong beat from
  // a weak one, which is what the tempo choice further down rests on:
  //
  //  - Magnitudes are log-compressed first. A chorus is many times louder than a verse but not
  //    many times more eventful, and without this the loud half of a song supplies all the beats.
  //
  //  - Each band contributes its *average* rise per bin rather than the total. A noisy cymbal
  //    spreads itself over hundreds of bins and a kick drum lives in ten, so summing bins raw
  //    makes a hi-hat measure as an event bigger than the bass drum under it - and then every
  //    eighth note looks exactly as strong as every quarter.
  constexpr size_t kBands = 4;
  const double kBandEdges[kBands + 1] = {30.0, 180.0, 700.0, 2800.0, 11000.0};
  // The beat is carried by the kit's bottom end far more than by its cymbals.
  const float kBandWeights[kBands] = {1.0f, 1.0f, 0.6f, 0.35f};
  constexpr float kCompression = 500.0f;

  std::vector<float> onset(windows, 0.0f);
  std::vector<float> previousMagnitude(kWindow / 2, 0.0f);
  std::vector<float> real(kWindow);
  std::vector<float> imag(kWindow);

  // Which band each bin belongs to, worked out once.
  std::vector<int> bandOfBin(kWindow / 2, -1);
  for (size_t bin = 1; bin < kWindow / 2; bin++)
  {
    const double hz = static_cast<double>(bin) * file.sampleRate / static_cast<double>(kWindow);
    for (size_t b = 0; b < kBands; b++)
      if (hz >= kBandEdges[b] && hz < kBandEdges[b + 1])
        bandOfBin[bin] = static_cast<int>(b);
  }

  // Hann, for the same reason the spectrum display has one: without it every note smears across
  // the spectrum and the flux stops meaning anything.
  std::vector<float> window(kWindow);
  for (size_t i = 0; i < kWindow; i++)
    window[i] = 0.5f * (1.0f - std::cos(6.2831853f * static_cast<float>(i) / static_cast<float>(kWindow - 1)));

  for (size_t w = 0; w < windows; w++)
  {
    const size_t start = w * kHop;
    for (size_t i = 0; i < kWindow; i++)
    {
      const size_t frame = start + i;
      const float mono = 0.5f * (file.samples[frame * 2 + 0] + file.samples[frame * 2 + 1]);
      real[i] = mono * window[i];
      imag[i] = 0.0f;
    }
    ForwardFft(real, imag);

    float bandRise[kBands] = {0.0f, 0.0f, 0.0f, 0.0f};
    int bandBins[kBands] = {0, 0, 0, 0};

    for (size_t bin = 1; bin < kWindow / 2; bin++)
    {
      const float magnitude =
        std::log1p(kCompression * std::sqrt(real[bin] * real[bin] + imag[bin] * imag[bin]));
      const float rise = std::max(0.0f, magnitude - previousMagnitude[bin]);
      previousMagnitude[bin] = magnitude;

      const int band = bandOfBin[bin];
      if (band < 0)
        continue;
      bandRise[band] += rise;
      bandBins[band]++;
    }

    float flux = 0.0f;
    for (size_t b = 0; b < kBands; b++)
      if (bandBins[b] > 0)
        flux += kBandWeights[b] * bandRise[b] / static_cast<float>(bandBins[b]);
    onset[w] = flux;
  }

  // Flatten out the long-term loudness of the track: a quiet intro should contribute beats just
  // as a loud chorus does. Subtracting a local mean leaves what stands out locally.
  constexpr size_t kSmoothing = 32;
  std::vector<float> normalised(windows, 0.0f);
  for (size_t w = 0; w < windows; w++)
  {
    const size_t from = (w > kSmoothing) ? w - kSmoothing : 0;
    const size_t to = std::min(windows, w + kSmoothing + 1);

    float sum = 0.0f;
    for (size_t i = from; i < to; i++)
      sum += onset[i];
    const float mean = sum / static_cast<float>(to - from);
    normalised[w] = std::max(0.0f, onset[w] - mean);
  }

  // --- tempo, by comb-filtering the autocorrelation ---
  //
  // Scoring the lag and its first two multiples together is what stops a strong bar-length
  // periodicity from being mistaken for the beat.
  std::vector<double> combScores;
  std::vector<double> combBpms;

  for (float bpm = kMinBpm; bpm <= kMaxBpm; bpm += kBpmStep)
  {
    const double lag = 60.0 / static_cast<double>(bpm) * frameRate;
    if (lag < 2.0 || lag * 3.0 >= static_cast<double>(windows))
      continue;

    double score = 0.0;
    for (int multiple = 1; multiple <= 3; multiple++)
    {
      const size_t offset = static_cast<size_t>(std::lround(lag * multiple));
      if (offset >= windows)
        break;

      double sum = 0.0;
      for (size_t w = 0; w + offset < windows; w++)
        sum += static_cast<double>(normalised[w]) * static_cast<double>(normalised[w + offset]);
      score += sum / static_cast<double>(multiple);
    }

    combScores.push_back(score);
    combBpms.push_back(static_cast<double>(bpm));
  }

  if (combScores.empty())
    return estimate;

  double bestScore = 0.0;
  double bestBpm = kPreferredBpm;
  double totalScore = 0.0;
  for (size_t i = 0; i < combScores.size(); i++)
  {
    // The preference still shapes the search, but only to rank which periodicities are worth
    // trying. What actually decides is further down, where each one is tracked and listened to.
    const double weighted = combScores[i] * TempoPreference(combBpms[i]);
    totalScore += weighted;
    if (weighted > bestScore)
    {
      bestScore = weighted;
      bestBpm = combBpms[i];
    }
  }

  // --- which pulse, and why this is not decided here ---
  //
  // Nothing in the signal settles it. A song with eighth notes running through it is exactly as
  // periodic at the eighths as at the quarters, and 84 and 168 are both true descriptions of the
  // same recording - which one is "the" tempo is a question about what a person would nod along
  // to, not about the audio. The preference above is that judgement, and like any judgement about
  // taste it is sometimes not the listener's.
  //
  // Measures that try to settle it acoustically were tried and thrown away: how much quieter the
  // midpoints are catches a grid running at half speed, but every other beat being weaker - which
  // ought to catch double speed - is just as true at the right tempo, because a kick and a snare
  // are different sounds. That is every rock beat ever recorded, not a mistake.
  //
  // So the honest thing is to pick the likeliest, track it accurately, and put halving and
  // doubling one button away in the UI - which is exact, instant, and always right, because the
  // person pressing it is the authority the analysis was guessing at.
  const std::vector<size_t> chosenBeats = TrackBeats(normalised, 60.0 / bestBpm * frameRate);

  // --- phase: where the first beat sits ---
  //
  // The tempo says how far apart the beats are; this says where to start counting. Without it a
  // grid at the right tempo can still land squarely between every beat.
  const double beatFrames = 60.0 / bestBpm * frameRate;
  double bestOffsetScore = -1.0;
  double bestOffset = 0.0;

  for (double offset = 0.0; offset < beatFrames; offset += 0.25)
  {
    double sum = 0.0;
    for (double position = offset; position < static_cast<double>(windows); position += beatFrames)
      sum += static_cast<double>(normalised[static_cast<size_t>(position)]);

    if (sum > bestOffsetScore)
    {
      bestOffsetScore = sum;
      bestOffset = offset;
    }
  }

  // The flux for a window is credited to where the window *starts*, but a window catches an onset
  // as soon as its span reaches it - so the rise begins up to a window before the beat, and every
  // beat is reported early by about half of one. A fixed bias rather than noise, so it is simply
  // added back. Measured at 10.8 ms against a half window of 11.6 ms, which is the same thing.
  const double onsetBias = static_cast<double>(kWindow) * 0.5 / file.sampleRate;

  estimate.bpm = static_cast<float>(bestBpm);
  estimate.firstBeatSeconds = bestOffset / frameRate + onsetBias;
  // How far the winner stood above the average candidate, squashed into something readable.
  const double mean = totalScore / static_cast<double>(combScores.size());
  estimate.confidence = mean > 0.0 ? static_cast<float>(std::clamp((bestScore / mean - 1.0) / 2.0, 0.0, 1.0)) : 0.0f;
  estimate.valid = true;

  // --- the beats themselves ---
  //
  // Everything above produced one tempo and one offset, which is where a rigid grid comes from.
  // This follows the song instead.
  const std::vector<size_t>& tracked = chosenBeats;
  if (tracked.size() >= 4)
  {
    estimate.beats.reserve(tracked.size());
    for (const size_t frame : tracked)
    {
      // Sub-frame position from the shape of the flux around the peak. A frame is 12 ms, which is
      // enough to hear on a click, and the parabola through three points costs nothing.
      double refined = static_cast<double>(frame);
      if (frame > 0 && frame + 1 < normalised.size())
      {
        const double a = normalised[frame - 1];
        const double b = normalised[frame];
        const double c = normalised[frame + 1];
        const double denominator = a - 2.0 * b + c;
        if (std::fabs(denominator) > 1.0e-9)
          refined += std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
      }
      estimate.beats.push_back(refined / frameRate + onsetBias);
    }

    estimate.firstBeatSeconds = estimate.beats.front();

    // The median interval, because a handful of doubled or dropped beats should not move the
    // number the user reads.
    std::vector<double> intervals;
    intervals.reserve(estimate.beats.size());
    for (size_t i = 1; i < estimate.beats.size(); i++)
      intervals.push_back(estimate.beats[i] - estimate.beats[i - 1]);

    std::vector<double> sorted = intervals;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const double median = sorted[sorted.size() / 2];
    if (median > 0.0)
      estimate.bpm = static_cast<float>(60.0 / median);

    // How far the tempo actually moved, measured over eight beats at a time so one loose snare
    // does not read as the song speeding up.
    constexpr size_t kSpan = 8;
    if (intervals.size() > kSpan)
    {
      double lowest = 1.0e9;
      double highest = 0.0;
      for (size_t i = 0; i + kSpan <= intervals.size(); i++)
      {
        double total = 0.0;
        for (size_t k = 0; k < kSpan; k++)
          total += intervals[i + k];
        const double local = 60.0 / (total / static_cast<double>(kSpan));
        lowest = std::min(lowest, local);
        highest = std::max(highest, local);
      }
      estimate.lowestBpm = static_cast<float>(lowest);
      estimate.highestBpm = static_cast<float>(highest);
    }

    // Which beat is a downbeat: the phase whose beats are loudest. A bar line drawn on the wrong
    // beat is worse than no bar line, and this is the only thing that can tell.
    constexpr int kBeatsPerBar = 4;
    double bestBarScore = -1.0;
    for (int phase = 0; phase < kBeatsPerBar; phase++)
    {
      double total = 0.0;
      int counted = 0;
      for (size_t i = static_cast<size_t>(phase); i < tracked.size(); i += kBeatsPerBar)
      {
        total += static_cast<double>(normalised[tracked[i]]);
        counted++;
      }
      const double average = counted > 0 ? total / static_cast<double>(counted) : 0.0;
      if (average > bestBarScore)
      {
        bestBarScore = average;
        estimate.downbeatOffset = phase;
      }
    }
  }

  return estimate;
}

} // namespace nam_ui


