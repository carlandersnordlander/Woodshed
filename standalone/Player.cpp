#include "Player.h"

#include <algorithm>
#include <cmath>

namespace nam_ui
{

namespace
{
float DbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}
} // namespace

void Player::Prepare(double sampleRate, unsigned int maxFrames)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
  mSnapshot.reserve(kMaxTracks);
  mPosition.store(0, std::memory_order_relaxed);

  // At the slowest setting a block needs a quarter of its length in input; at the fastest, double.
  // Sized for the fastest, so the read buffer is never the thing that fails.
  const size_t maxInput = static_cast<size_t>(static_cast<float>(maxFrames) * kMaxPlaybackSpeed) + 8;
  mMixLeft.assign(maxInput, 0.0f);
  mMixRight.assign(maxInput, 0.0f);
  mOutLeft.assign(maxFrames + 8, 0.0f);
  mOutRight.assign(maxFrames + 8, 0.0f);

  mStretch.presetDefault(2, static_cast<float>(mSampleRate));
  mStretch.reset();
  mAppliedSemitones = 0.0f;
  mWasStretching = false;

  mSynth.Prepare(mSampleRate);
}

int Player::AddTrack(std::shared_ptr<AudioFile> file, std::shared_ptr<PeakCache> peaks)
{
  std::lock_guard<std::mutex> lock(mMutex);
  if (mTracks.size() >= kMaxTracks || !file)
    return -1;

  Track track;
  track.id = mNextTrackId++;
  track.file = std::move(file);
  track.peaks = std::move(peaks);
  track.name = track.file->name;
  mTracks.push_back(std::move(track));
  return mTracks.back().id;
}

void Player::RemoveTrack(int id)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mTracks.erase(std::remove_if(mTracks.begin(), mTracks.end(), [id](const Track& t) { return t.id == id; }),
                mTracks.end());
}

void Player::MoveTrack(int id, size_t newIndex)
{
  std::lock_guard<std::mutex> lock(mMutex);

  const auto at = std::find_if(mTracks.begin(), mTracks.end(), [id](const Track& t) { return t.id == id; });
  if (at == mTracks.end())
    return;

  const size_t from = static_cast<size_t>(std::distance(mTracks.begin(), at));
  const size_t to = std::min(newIndex, mTracks.size() - 1);
  if (from == to)
    return;

  Track moved = std::move(*at);
  mTracks.erase(at);
  mTracks.insert(mTracks.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));

  // The filter slots are matched to tracks by id and reset themselves when a slot changes hands,
  // so a reorder needs nothing here beyond letting that happen.
}

void Player::Clear()
{
  std::lock_guard<std::mutex> lock(mMutex);
  mTracks.clear();
  mLoops.clear();
  mActiveLoop.store(-1, std::memory_order_relaxed);
  mPosition.store(0, std::memory_order_relaxed);
  mPlaying.store(false, std::memory_order_relaxed);
}

std::vector<Track> Player::GetTracks() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mTracks;
}

void Player::SetTrack(const Track& track)
{
  std::lock_guard<std::mutex> lock(mMutex);
  for (auto& existing : mTracks)
    if (existing.id == track.id)
    {
      // The file and its peaks are not the caller's to change - only what is done with them.
      existing.name = track.name;
      existing.gainDb = track.gainDb;
      existing.muted = track.muted;
      existing.soloed = track.soloed;
      existing.eqEnabled = track.eqEnabled;
      existing.eq = track.eq;
    }
}

double Player::DurationSeconds() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  double longest = 0.0;
  for (const auto& track : mTracks)
    if (track.file)
      longest = std::max(longest, track.file->DurationSeconds());
  return longest;
}

void Player::Play()
{
  mPlaying.store(true, std::memory_order_relaxed);
  // Starting in the middle of a held chord should sound it, not wait for the next one.
  mSynth.Invalidate();
}

void Player::Pause()
{
  mPlaying.store(false, std::memory_order_relaxed);
  // Otherwise a piano note struck just before you stopped hangs on over the silence.
  mSynth.AllNotesOff();
}

void Player::TogglePlay()
{
  const bool playing = !mPlaying.load(std::memory_order_relaxed);
  mPlaying.store(playing, std::memory_order_relaxed);
  if (playing)
    mSynth.Invalidate();
  else
    mSynth.AllNotesOff();
}

void Player::Stop()
{
  mPlaying.store(false, std::memory_order_relaxed);
  mSynth.AllNotesOff();

  // Back to the top of whatever you are working on, which is the loop if there is one. Stopping
  // inside a loop and landing at the start of the song would be the wrong place every time.
  double target = 0.0;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    const int active = mActiveLoop.load(std::memory_order_relaxed);
    if (active >= 0 && static_cast<size_t>(active) < mLoops.size())
      target = mLoops[static_cast<size_t>(active)].startSeconds;
  }
  SetPositionSeconds(target);
}

double Player::GetPositionSeconds() const
{
  return static_cast<double>(mPosition.load(std::memory_order_relaxed)) / mSampleRate;
}

void Player::SetPositionSeconds(double seconds)
{
  mPosition.store(static_cast<long long>(std::max(0.0, seconds) * mSampleRate), std::memory_order_relaxed);

  // Every seek goes through here - clicking the waveform, stepping to a loop, going back to the
  // top - so this is the one place the synth has to be told the playhead moved.
  mSynth.AllNotesOff();
  mSynth.Invalidate();
}

std::vector<Loop> Player::GetLoops() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mLoops;
}

void Player::SetLoops(std::vector<Loop> loops)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mLoops = std::move(loops);
  if (mActiveLoop.load(std::memory_order_relaxed) >= static_cast<int>(mLoops.size()))
    mActiveLoop.store(-1, std::memory_order_relaxed);
}

void Player::SetActiveLoop(int index)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mSynth.Invalidate(); // the loop that is running decides where the playhead goes next

  if (index < 0 || static_cast<size_t>(index) >= mLoops.size())
  {
    mActiveLoop.store(-1, std::memory_order_relaxed);
    return;
  }
  mActiveLoop.store(index, std::memory_order_relaxed);
}

void Player::Process(float* output, unsigned int frames)
{
  // A note pressed in the timeline sounds whether or not anything is playing - which is when you
  // are most likely to be looking at one. Collected before the early return, so the transport
  // being stopped is not the same thing as the synth being switched off.
  mSynth.CollectAuditions();

  if (!mPlaying.load(std::memory_order_relaxed))
  {
    if (mSynth.Sounding())
    {
      const float idleGain = DbToLinear(mGainDb.load(std::memory_order_relaxed));
      for (unsigned int i = 0; i < frames; i++)
      {
        float left = 0.0f;
        float right = 0.0f;
        mSynth.RenderIdle(left, right);
        output[i * 2 + 0] += left * idleGain;
        output[i * 2 + 1] += right * idleGain;
      }
    }
    return;
  }

  // Copied out from under the lock, like the block chain: the mixing itself runs unlocked.
  long long loopStart = -1;
  long long loopEnd = -1;
  bool anySolo = false;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot.assign(mTracks.begin(), mTracks.end());

    const int active = mActiveLoop.load(std::memory_order_relaxed);
    if (active >= 0 && static_cast<size_t>(active) < mLoops.size())
    {
      const Loop& loop = mLoops[static_cast<size_t>(active)];
      if (loop.Valid())
      {
        loopStart = static_cast<long long>(loop.startSeconds * mSampleRate);
        loopEnd = static_cast<long long>(loop.endSeconds * mSampleRate);
      }
    }
  }

  for (const auto& track : mSnapshot)
    if (track.soloed)
      anySolo = true;

  // Soloing the click silences the tracks without disturbing what any of them is set to, so
  // letting go of it puts the mix back exactly as it was.
  const bool silenced = mSilenced.load(std::memory_order_relaxed);

  const float masterGain = DbToLinear(mGainDb.load(std::memory_order_relaxed));
  const float speed = std::clamp(mSpeed.load(std::memory_order_relaxed), kMinPlaybackSpeed, kMaxPlaybackSpeed);
  const float semitones = mSemitones.load(std::memory_order_relaxed);

  // At speed 1 with no transposition the stretcher is not in the path at all: normal playback
  // then costs nothing and has no added latency, which is how it is most of the time.
  const bool stretching = (std::fabs(speed - 1.0f) > 1.0e-4f) || (std::fabs(semitones) > 1.0e-4f);
  if (stretching && !mWasStretching)
    mStretch.reset(); // coming in cold, rather than with a tail from minutes ago
  mWasStretching = stretching;

  if (stretching && semitones != mAppliedSemitones)
  {
    mStretch.setTransposeFactor(std::pow(2.0f, semitones / 12.0f));
    mAppliedSemitones = semitones;
  }

  // How much of the recording this block consumes. Slower means less input for the same output.
  const unsigned int inputFrames =
    stretching ? static_cast<unsigned int>(std::lround(static_cast<double>(frames) * static_cast<double>(speed)))
               : frames;
  const unsigned int readFrames = std::min<unsigned int>(inputFrames, static_cast<unsigned int>(mMixLeft.size()));

  // One filter slot per lane, matched by id: a slot that has been handed a different track starts
  // from silence rather than ringing with the last one's tail.
  for (size_t t = 0; t < mSnapshot.size() && t < mTrackFilters.size(); t++)
  {
    TrackFilter& filter = mTrackFilters[t];
    if (filter.trackId != mSnapshot[t].id)
    {
      filter.trackId = mSnapshot[t].id;
      filter.eq.Reset();
    }
    if (mSnapshot[t].eqEnabled)
      filter.eq.SetSettings(mSnapshot[t].eq, mSampleRate);
  }

  // The synth follows the player's own transposition: shift a song down two semitones and the
  // notes drawn under it have to come down with it, or they argue.
  mSynth.SetTranspose(semitones);
  const bool synthPlaying = mSynth.BeginBlock();

  long long position = mPosition.load(std::memory_order_relaxed);

  // A loop is honoured per sample rather than per block, so the seam lands exactly where the
  // marker is instead of up to a buffer late - which at a loop point is audible as a stutter.
  for (unsigned int i = 0; i < readFrames; i++)
  {
    if (loopEnd > loopStart && position >= loopEnd)
      position = loopStart;

    float left = 0.0f;
    float right = 0.0f;

    for (size_t t = 0; t < mSnapshot.size(); t++)
    {
      const Track& track = mSnapshot[t];
      if (silenced || !track.file || track.muted)
        continue;
      if (anySolo && !track.soloed)
        continue;

      const size_t frameCount = track.file->FrameCount();
      if (position < 0 || static_cast<size_t>(position) >= frameCount)
        continue;

      float trackLeft = track.file->samples[static_cast<size_t>(position) * 2 + 0];
      float trackRight = track.file->samples[static_cast<size_t>(position) * 2 + 1];

      // EQ first, then the fader - so moving a level does not change the tone, which is the whole
      // reason a mixer is arranged that way.
      if (track.eqEnabled && t < mTrackFilters.size())
        mTrackFilters[t].eq.ProcessSample(trackLeft, trackRight);

      const float gain = DbToLinear(track.gainDb);
      left += trackLeft * gain;
      right += trackRight * gain;
    }

    // Into the same buffer as the tracks, at the same position, so everything downstream treats
    // the two identically.
    //
    // A note pressed in the timeline sounds even when there is no score to play - having found the
    // notes and not switched playback on is exactly when you would be pressing them to check.
    if (synthPlaying)
      mSynth.RenderSample(position, left, right);
    else if (mSynth.Sounding())
      mSynth.RenderIdle(left, right);

    mMixLeft[i] = left;
    mMixRight[i] = right;
    position++;
  }

  if (synthPlaying)
    mSynth.EndBlock();

  // --- the end of the song is the end of playing ---
  //
  // Outside a loop the cursor used to run on past the last stem for as long as you left it, every
  // track skipped for being out of range, so the transport went on saying it was playing while
  // nothing came out. Held exactly at the end, not wound back: stopping is not rewinding.
  if (loopEnd <= loopStart)
  {
    long long lastFrame = 0;
    for (const auto& track : mSnapshot)
      if (track.file)
        lastFrame = std::max(lastFrame, static_cast<long long>(track.file->FrameCount()));

    if (lastFrame > 0 && position >= lastFrame)
    {
      position = lastFrame;
      mPlaying.store(false, std::memory_order_relaxed);
      mSynth.AllNotesOff();
    }
  }

  if (!stretching)
  {
    for (unsigned int i = 0; i < readFrames; i++)
    {
      output[i * 2 + 0] += mMixLeft[i] * masterGain;
      output[i * 2 + 1] += mMixRight[i] * masterGain;
    }
  }
  else
  {
    float* inputs[2] = {mMixLeft.data(), mMixRight.data()};
    float* outputs[2] = {mOutLeft.data(), mOutRight.data()};
    mStretch.process(inputs, static_cast<int>(readFrames), outputs, static_cast<int>(frames));

    for (unsigned int i = 0; i < frames; i++)
    {
      output[i * 2 + 0] += mOutLeft[i] * masterGain;
      output[i * 2 + 1] += mOutRight[i] * masterGain;
    }
  }

  mPosition.store(position, std::memory_order_relaxed);
}

} // namespace nam_ui
