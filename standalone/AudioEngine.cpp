#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace nam_ui
{

namespace
{
constexpr float kSilenceFloorDb = -100.0f;

float DbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

float LinearToDb(float linear)
{
  if (linear <= 0.0f)
    return kSilenceFloorDb;
  return std::max(kSilenceFloorDb, 20.0f * std::log10(linear));
}
} // namespace

AudioEngine::~AudioEngine()
{
  Close();
}

std::vector<RtAudio::Api> AudioEngine::GetCompiledApis() const
{
  std::vector<RtAudio::Api> apis;
  RtAudio::getCompiledApi(apis);
  return apis;
}

std::vector<RtAudio::DeviceInfo> AudioEngine::GetDevices(RtAudio::Api api) const
{
  mLastProbeMessage.clear();
  RtAudio probe(api, [this](RtAudioErrorType, const std::string& message) { mLastProbeMessage = message; });
  std::vector<RtAudio::DeviceInfo> devices;
  for (unsigned int id : probe.getDeviceIds())
    devices.push_back(probe.getDeviceInfo(id));
  return devices;
}

std::string AudioEngine::Open(const AudioDeviceChoice& choice)
{
  Close();

  mRtAudio = std::make_unique<RtAudio>(choice.api);
  if (mRtAudio->getDeviceIds().empty())
  {
    mLastError = "No audio devices found for the selected API.";
    mRtAudio.reset();
    return mLastError;
  }

  // Clamped against what the device actually has: a stored channel from a different interface must
  // not be able to make openStream fail on a machine where that socket does not exist.
  const RtAudio::DeviceInfo outInfo = mRtAudio->getDeviceInfo(choice.outputDeviceId);
  const RtAudio::DeviceInfo inInfo = mRtAudio->getDeviceInfo(choice.inputDeviceId);

  RtAudio::StreamParameters outParams;
  outParams.deviceId = choice.outputDeviceId;
  outParams.nChannels = 2;
  outParams.firstChannel = (outInfo.outputChannels >= 2)
                             ? std::min(choice.outputFirstChannel, outInfo.outputChannels - 2)
                             : 0;

  RtAudio::StreamParameters inParams;
  inParams.deviceId = choice.inputDeviceId;
  inParams.nChannels = 1;
  inParams.firstChannel =
    (inInfo.inputChannels >= 1) ? std::min(choice.inputFirstChannel, inInfo.inputChannels - 1) : 0;

  unsigned int bufferFrames = choice.bufferFrames;
  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_SCHEDULE_REALTIME;

  const RtAudioErrorType openResult = mRtAudio->openStream(&outParams, &inParams, RTAUDIO_FLOAT32, choice.sampleRate,
                                                           &bufferFrames, &AudioEngine::AudioCallback, this, &options);
  if (openResult != RTAUDIO_NO_ERROR)
  {
    mLastError = "Failed to open audio stream (error code " + std::to_string(static_cast<int>(openResult)) + ").";
    mRtAudio.reset();
    return mLastError;
  }

  const size_t scratchSize = std::max<size_t>(bufferFrames, kMaxBlockFrames);
  for (size_t i = 0; i < mScratch.size(); i++)
  {
    mScratch[i].assign(scratchSize, NAM_SAMPLE(0));
    mScratchPtrs[i] = {mScratch[i].data()};
  }
  mDoubleScratch.assign(scratchSize, 0.0);
  mDoubleScratchPtrs = {mDoubleScratch.data()};

  // Reserved once so the audio thread's snapshot of the chain never allocates.
  mChainSnapshot.reserve(kMaxBlocks);

  const double streamRate = static_cast<double>(mRtAudio->getStreamSampleRate());
  mStreamSampleRate = streamRate;
  mTuner.Prepare(streamRate);

  // Size every crossover's buffers here, and force a coefficient rebuild on the next block.
  {
    std::vector<double> silence(scratchSize, 0.0);
    double* channel = silence.data();
    double** silentInput = &channel;
    for (size_t s = 0; s < kMaxParallelSections; s++)
    {
      mAppliedCrossoverHz[s] = 0.0f;
      mCrossoverLow[s].SetParams(recursive_linear_filter::LowPassParams(streamRate, 500.0));
      mCrossoverHigh[s].SetParams(recursive_linear_filter::HighPassParams(streamRate, 500.0));
      mCrossoverLow[s].Process(silentInput, 1, scratchSize);
      mCrossoverHigh[s].Process(silentInput, 1, scratchSize);
    }
  }

  for (auto& eq : mEqByPosition)
    eq.Prepare(streamRate, static_cast<int>(scratchSize));

  // Same treatment as the crossovers: size every cut's buffers now and force a coefficient
  // rebuild, so the first block a cut ever processes does not allocate on the audio thread.
  {
    std::vector<double> silence(scratchSize, 0.0);
    double* channel = silence.data();
    double** silentInput = &channel;
    for (auto& cut : mCutByPosition)
    {
      cut.appliedLowHz = -1.0f;
      cut.appliedHighHz = -1.0f;
      for (auto& section : cut.low)
      {
        section.SetParams(recursive_linear_filter::HighPassParams(streamRate, 100.0));
        section.Process(silentInput, 1, scratchSize);
      }
      for (auto& section : cut.high)
      {
        section.SetParams(recursive_linear_filter::LowPassParams(streamRate, 5000.0));
        section.Process(silentInput, 1, scratchSize);
      }
    }
  }
  for (auto& level : mBlockLevels)
    level.store(kSilenceFloorDb, std::memory_order_relaxed);
  for (auto& reduction : mBlockGainReduction)
    reduction.store(0.0f, std::memory_order_relaxed);
  for (auto& comp : mCompByPosition)
    comp.Prepare(streamRate);
  for (auto& peq : mPeqByPosition)
    peq.Prepare(streamRate, static_cast<int>(scratchSize));
  mMetronome.Prepare(streamRate);
  mPlayer.Prepare(streamRate, static_cast<unsigned int>(scratchSize));

  // Wiring the Gain up as a listener is what lets the trigger analyse the input while the
  // attenuation is applied somewhere else; inserting twice is harmless.
  mGateTrigger.SetSampleRate(streamRate);
  mGateTrigger.AddListener(&mGateGain);
  mGateParamsApplied = false;
  {
    std::vector<double> silence(scratchSize, 0.0);
    double* channel = silence.data();
    double** silentInput = &channel;
    mGateTrigger.Process(silentInput, 1, scratchSize);
    mGateGain.Process(silentInput, 1, scratchSize);
  }

  const RtAudioErrorType startResult = mRtAudio->startStream();
  if (startResult != RTAUDIO_NO_ERROR)
  {
    mLastError = "Failed to start audio stream (error code " + std::to_string(static_cast<int>(startResult)) + ").";
    mRtAudio->closeStream();
    mRtAudio.reset();
    return mLastError;
  }

  mActiveChoice = choice;
  mActiveChoice.bufferFrames = bufferFrames;
  mActiveChoice.sampleRate = mRtAudio->getStreamSampleRate();
  mUnderrunCount.store(0, std::memory_order_relaxed);
  mIsOpen = true;
  mLastError.clear();
  return {};
}

void AudioEngine::Close()
{
  if (mRtAudio)
  {
    if (mRtAudio->isStreamRunning())
      mRtAudio->stopStream();
    if (mRtAudio->isStreamOpen())
      mRtAudio->closeStream();
    mRtAudio.reset();
  }
  mIsOpen = false;
}

size_t AudioEngine::FindBlock(int id) const
{
  for (size_t i = 0; i < mChain.size(); i++)
    if (mChain[i].id == id)
      return i;
  return std::string::npos;
}

size_t ChainRouting::SectionAt(size_t blockIndex) const
{
  for (size_t s = 0; s < sectionCount; s++)
    if (blockIndex >= sections[s].splitIndex && blockIndex < sections[s].mergeIndex)
      return s;
  return kNoSection;
}

void AudioEngine::ValidateRoutingLocked()
{
  const size_t count = mChain.size();

  // Clamp first, then order: everything below assumes the sections are sorted and in range.
  for (size_t s = 0; s < mRouting.sectionCount; s++)
  {
    ParallelSection& section = mRouting.sections[s];
    section.splitIndex = std::min(section.splitIndex, count);
    section.mergeIndex = std::clamp(section.mergeIndex, section.splitIndex, count);
  }
  std::sort(mRouting.sections.begin(), mRouting.sections.begin() + static_cast<std::ptrdiff_t>(mRouting.sectionCount),
            [](const ParallelSection& a, const ParallelSection& b) { return a.splitIndex < b.splitIndex; });

  size_t kept = 0;
  size_t previousEnd = 0;
  for (size_t s = 0; s < mRouting.sectionCount; s++)
  {
    ParallelSection section = mRouting.sections[s];

    // Sections never overlap: one starts no earlier than the last one ended. Dragging a handle
    // across a neighbour therefore squeezes rather than corrupts.
    section.splitIndex = std::max(section.splitIndex, previousEnd);
    section.mergeIndex = std::max(section.mergeIndex, section.splitIndex);

    // A branch with nothing on it is not a branch. Removing the last block from the lower row -
    // or moving the split past it - dissolves the section rather than leaving it stranded.
    bool anyLower = false;
    for (size_t i = section.splitIndex; i < section.mergeIndex && i < count; i++)
      if (mChain[i].settings.row == 1)
        anyLower = true;

    if (!anyLower || section.mergeIndex <= section.splitIndex)
      continue;

    mRouting.sections[kept++] = section;
    previousEnd = section.mergeIndex;
  }
  mRouting.sectionCount = std::min(kept, kMaxParallelSections);

  // A block on a lower branch has to be inside a section - outside one there is no lower line for
  // it to be on. So a section that would leave one behind stretches back over it instead, which is
  // what stops a merge being dragged left past the last block on its own lower branch.
  for (size_t i = 0; i < count; i++)
  {
    if (mChain[i].settings.row != 1 || mRouting.SectionAt(i) != kNoSection)
      continue;

    bool covered = false;
    for (size_t s = 0; s < mRouting.sectionCount && !covered; s++)
    {
      ParallelSection& section = mRouting.sections[s];
      if (i >= section.mergeIndex)
      {
        // Reaching forwards, but never into the next section: they are not allowed to overlap.
        const size_t limit = (s + 1 < mRouting.sectionCount) ? mRouting.sections[s + 1].splitIndex : count;
        if (i < limit)
        {
          section.mergeIndex = i + 1;
          covered = true;
        }
      }
      else if (i < section.splitIndex)
      {
        const size_t limit = (s > 0) ? mRouting.sections[s - 1].mergeIndex : 0;
        if (i >= limit)
        {
          section.splitIndex = i;
          covered = true;
        }
      }
    }

    // Nowhere to put it - no section can reach without running into another one. Being honest
    // about that beats drawing a branch the signal never travels down.
    if (!covered)
      mChain[i].settings.row = 0;
  }
}

int AudioEngine::AddBlock(BlockType type)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  if (mChain.size() >= kMaxBlocks)
    return -1;

  Block block;
  block.id = mNextBlockId++;
  block.settings.type = type;
  mChain.push_back(std::move(block));
  ValidateRoutingLocked();
  return mChain.back().id;
}

void AudioEngine::RemoveBlock(int id)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index != std::string::npos)
  {
    mChain.erase(mChain.begin() + static_cast<std::ptrdiff_t>(index));

    // Same reasoning as in ReorderBlock: closing a gap ahead of a section's bound pulls it back
    // a step, and a bound behind the gap stays where it is.
    for (size_t s = 0; s < mRouting.sectionCount; s++)
    {
      if (index < mRouting.sections[s].splitIndex)
        mRouting.sections[s].splitIndex--;
      if (index < mRouting.sections[s].mergeIndex)
        mRouting.sections[s].mergeIndex--;
    }
  }
  ValidateRoutingLocked();
}

void AudioEngine::MoveBlock(int id, int delta)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos)
    return;

  const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(index) + delta;
  if (target < 0 || target >= static_cast<std::ptrdiff_t>(mChain.size()))
    return;
  std::swap(mChain[index], mChain[static_cast<size_t>(target)]);
  ValidateRoutingLocked();
}

void AudioEngine::ReorderBlock(int id, size_t newIndex)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos || newIndex >= mChain.size() || index == newIndex)
    return;

  Block block = std::move(mChain[index]);
  mChain.erase(mChain.begin() + static_cast<std::ptrdiff_t>(index));
  mChain.insert(mChain.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(block));

  // A section's bounds are positions in this same list, so they have to travel with the move.
  // Without this, dropping a block into a section shifts everything behind it one step, which
  // pushes the section's own contents out through its merge - and a branch left with nothing on
  // it dissolves. That is what limited a branch to a single block.
  const auto remap = [&](size_t boundary)
  {
    // Erasing the block closes a gap ahead of the boundary; re-inserting it opens one, but only
    // if it lands strictly before. Landing exactly on the boundary means landing inside the
    // range that starts there.
    size_t moved = boundary - (index < boundary ? 1 : 0);
    if (newIndex < moved)
      moved++;
    return moved;
  };

  for (size_t s = 0; s < mRouting.sectionCount; s++)
  {
    mRouting.sections[s].splitIndex = remap(mRouting.sections[s].splitIndex);
    mRouting.sections[s].mergeIndex = remap(mRouting.sections[s].mergeIndex);
  }

  ValidateRoutingLocked();
}

size_t AudioEngine::GetBlockCount() const
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  return mChain.size();
}

std::vector<Block> AudioEngine::GetBlocks() const
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  return mChain;
}

bool AudioEngine::GetBlockSettings(int id, BlockSettings& out) const
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos)
    return false;
  out = mChain[index].settings;
  return true;
}

void AudioEngine::SetBlockSettings(int id, const BlockSettings& settings)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos)
    return;

  // Changing a block's type leaves the old processor behind; drop it so the block reads as empty.
  if (mChain[index].settings.type != settings.type)
  {
    mChain[index].namModel.reset();
    mChain[index].ir.reset();
  }
  mChain[index].settings = settings;
  // A row change can empty the lower branch, which is a reason for the section to go away.
  ValidateRoutingLocked();
}

void AudioEngine::SetBlockRow(int id, int row)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos || mChain[index].settings.row == row)
    return;

  mChain[index].settings.row = row;

  if (row == 1 && mRouting.SectionAt(index) == kNoSection)
  {
    // Dragging a block onto the lower lane is what creates a parallel section - there is no
    // separate switch for it. A section that ends or starts right beside the block takes it in
    // instead, so two sections never end up back to back with nothing between them.
    bool grown = false;
    for (size_t s = 0; s < mRouting.sectionCount && !grown; s++)
    {
      if (mRouting.sections[s].mergeIndex == index)
      {
        mRouting.sections[s].mergeIndex = index + 1;
        grown = true;
      }
      else if (mRouting.sections[s].splitIndex == index + 1)
      {
        mRouting.sections[s].splitIndex = index;
        grown = true;
      }
    }

    if (!grown && mRouting.sectionCount < kMaxParallelSections)
    {
      ParallelSection section;
      section.splitIndex = index;
      section.mergeIndex = index + 1;
      mRouting.sections[mRouting.sectionCount++] = section;
    }
    else if (!grown)
    {
      // Out of sections. The block stays on the main line rather than sitting on a branch that
      // nothing would ever process.
      mChain[index].settings.row = 0;
    }
  }

  ValidateRoutingLocked();
}

void AudioEngine::ExtendSectionOver(int id, int otherId)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  const size_t other = FindBlock(otherId);
  if (index == std::string::npos || other == std::string::npos)
    return;

  const size_t s = mRouting.SectionAt(index);
  if (s == kNoSection || mRouting.SectionAt(other) == s)
    return;

  size_t split = std::min(mRouting.sections[s].splitIndex, other);
  size_t merge = std::max(mRouting.sections[s].mergeIndex, other + 1);

  // Only over ground no other section holds - they are not allowed to overlap, so a neighbour is
  // where this one stops growing.
  for (size_t t = 0; t < mRouting.sectionCount; t++)
  {
    if (t == s)
      continue;
    if (mRouting.sections[t].mergeIndex <= mRouting.sections[s].splitIndex)
      split = std::max(split, mRouting.sections[t].mergeIndex);
    if (mRouting.sections[t].splitIndex >= mRouting.sections[s].mergeIndex)
      merge = std::min(merge, mRouting.sections[t].splitIndex);
  }

  if (split <= index && merge > index)
  {
    mRouting.sections[s].splitIndex = split;
    mRouting.sections[s].mergeIndex = merge;
  }
  ValidateRoutingLocked();
}

void AudioEngine::SetBlockNamModel(int id, std::shared_ptr<nam::DSP> model)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos)
    return;
  mChain[index].namModel = std::move(model);
  mChain[index].ir.reset();
}

void AudioEngine::SetBlockIr(int id, std::shared_ptr<dsp::ImpulseResponse> ir)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos)
    return;
  mChain[index].ir = std::move(ir);
  mChain[index].namModel.reset();
}

void AudioEngine::ClearBlockProcessor(int id)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  const size_t index = FindBlock(id);
  if (index == std::string::npos)
    return;
  mChain[index].namModel.reset();
  mChain[index].ir.reset();
}

float AudioEngine::GetBlockLevelDb(size_t index) const
{
  return index < kMaxBlocks ? mBlockLevels[index].load(std::memory_order_relaxed) : kSilenceFloorDb;
}

float AudioEngine::GetBlockGainReductionDb(size_t index) const
{
  return index < kMaxBlocks ? mBlockGainReduction[index].load(std::memory_order_relaxed) : 0.0f;
}

ChainRouting AudioEngine::GetRouting() const
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  return mRouting;
}

void AudioEngine::SetRouting(const ChainRouting& routing)
{
  std::lock_guard<std::mutex> lock(mChainMutex);
  mRouting = routing;
  ValidateRoutingLocked();
}

void AudioEngine::SetMetronome(const MetronomeSettings& settings)
{
  std::lock_guard<std::mutex> lock(mMetronomeMutex);
  mMetronomeSettings = settings;
}

MetronomeSettings AudioEngine::GetMetronome() const
{
  std::lock_guard<std::mutex> lock(mMetronomeMutex);
  return mMetronomeSettings;
}

void AudioEngine::SetGate(const GateSettings& settings)
{
  mGateEnabled.store(settings.enabled, std::memory_order_relaxed);
  mGatePlacement.store(static_cast<int>(settings.placement), std::memory_order_relaxed);
  mGateThresholdDb.store(settings.thresholdDb, std::memory_order_relaxed);
}

GateSettings AudioEngine::GetGate() const
{
  GateSettings settings;
  settings.enabled = mGateEnabled.load(std::memory_order_relaxed);
  settings.placement = static_cast<GatePlacement>(mGatePlacement.load(std::memory_order_relaxed));
  settings.thresholdDb = mGateThresholdDb.load(std::memory_order_relaxed);
  return settings;
}

int AudioEngine::AudioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames, double /*streamTime*/,
                               RtAudioStreamStatus status, void* userData)
{
  auto* engine = static_cast<AudioEngine*>(userData);
  return engine->Process(static_cast<float*>(outputBuffer), static_cast<const float*>(inputBuffer), nFrames, status);
}

int AudioEngine::Process(float* output, const float* input, unsigned int nFrames, RtAudioStreamStatus status)
{
  if (status & (RTAUDIO_INPUT_OVERFLOW | RTAUDIO_OUTPUT_UNDERFLOW))
    mUnderrunCount.fetch_add(1, std::memory_order_relaxed);

  // Defensive clamp: some backends can call back with a different frame count than negotiated.
  const unsigned int frames = std::min<unsigned int>(nFrames, static_cast<unsigned int>(mScratch[0].size()));

  const float inputGain = DbToLinear(mInputGainDb.load(std::memory_order_relaxed));
  const float outputGain = DbToLinear(mOutputGainDb.load(std::memory_order_relaxed));

  float inputPeak = 0.0f;
  for (unsigned int i = 0; i < frames; i++)
  {
    const float sample = (input != nullptr ? input[i] : 0.0f) * inputGain;
    mScratch[0][i] = static_cast<NAM_SAMPLE>(sample);
    inputPeak = std::max(inputPeak, std::fabs(sample));
  }
  mInputLevelDb.store(LinearToDb(inputPeak), std::memory_order_relaxed);

  // Analyse before anything shapes the signal: the strobe reads phase, and distortion or EQ ahead
  // of it would smear exactly the thing it measures.
  if (mTunerEnabled.load(std::memory_order_relaxed))
    mTuner.Process(mScratch[0].data(), frames);

  // Copy the chain out from under the lock; the processing itself runs unlocked. The vector has
  // its capacity reserved in Open(), so this assigns into existing storage.
  {
    std::lock_guard<std::mutex> lock(mChainMutex);
    mChainSnapshot.assign(mChain.begin(), mChain.end());
  }

  const bool bypassed = mBypassed.load(std::memory_order_relaxed);

  // Buffer 0 holds the signal so far; the rest are working space for the branches.
  size_t current = 0;

  // AudioDSPTools runs in double while the rest of the chain is float, so its modules are wrapped
  // in a convert-in / convert-out step against the shared double buffer. Takes the buffer to work
  // on explicitly, because the parallel branches each have their own.
  const auto runInDoubleOn = [&](size_t buffer, auto&& invoke)
  {
    for (unsigned int i = 0; i < frames; i++)
      mDoubleScratch[i] = static_cast<double>(mScratch[buffer][i]);

    double* result = invoke(mDoubleScratchPtrs.data(), static_cast<size_t>(frames));

    for (unsigned int i = 0; i < frames; i++)
      mScratch[buffer][i] = static_cast<NAM_SAMPLE>(result[i]);
  };

  const auto runInDouble = [&](auto&& invoke) { runInDoubleOn(current, invoke); };

  // The gate runs its detection here, on the clean input, whichever end of the chain the
  // attenuation is applied at. Gating off a distorted signal chatters, and the threshold would
  // stop meaning anything about how hard you actually played.
  const GateSettings gate = GetGate();
  const bool gateOn = !bypassed && gate.enabled;
  if (gateOn)
  {
    if (!mGateParamsApplied || gate.thresholdDb != mAppliedGateThresholdDb)
    {
      // Everything but the threshold keeps the AudioDSPTools defaults, which are what the
      // official plugin ships: 50 ms detection, ratio 1.5, 2 ms open, 50 ms hold, 50 ms close.
      mGateTrigger.SetParams(
        dsp::noise_gate::TriggerParams(0.05, static_cast<double>(gate.thresholdDb), 1.5, 0.002, 0.050, 0.050));
      mAppliedGateThresholdDb = gate.thresholdDb;
      mGateParamsApplied = true;
    }

    for (unsigned int i = 0; i < frames; i++)
      mDoubleScratch[i] = static_cast<double>(mScratch[current][i]);
    // The trigger passes audio through unchanged; we only want the gain reduction it hands its
    // listener, so the result is deliberately ignored.
    mGateTrigger.Process(mDoubleScratchPtrs.data(), 1, static_cast<size_t>(frames));
  }

  const auto applyGate = [&]()
  { runInDouble([&](double** in, size_t n) { return mGateGain.Process(in, 1, n)[0]; }); };

  if (gateOn && gate.placement == GatePlacement::Pre)
    applyGate();

  // One block, run on a nominated buffer pair. Returns the buffer holding the result, which may
  // be either of the two depending on whether the block's processor swapped them.
  const auto runOneBlock = [&](size_t index, size_t src, size_t tmp) -> size_t
  {
    const Block& block = mChainSnapshot[index];
    const bool blockOn = !bypassed && block.settings.enabled;
    size_t result = src;

    // Tapped before anything this block does, so an EQ block's spectrum shows what it is being
    // asked to shape rather than the result of shaping it.
    if (block.id == mSpectrumBlockId.load(std::memory_order_relaxed))
      mSpectrum.Write(mScratch[result].data(), frames);

    const EqSettings& eq = block.settings.eq;
    const bool runEq = blockOn && eq.enabled;
    if (runEq)
      mEqByPosition[index].ApplyIfChanged(eq);

    const auto applyEq = [&](size_t buffer)
    { runInDoubleOn(buffer, [&](double** in, size_t n) { return mEqByPosition[index].Process(in, n); }); };

    if (runEq && eq.placement == EqPlacement::Pre)
      applyEq(result);

    bool ran = false;
    const float blend = block.settings.dryBlend;

    // Drive into whatever this block holds. Applied here rather than folded into the block's
    // volume, because where it sits relative to the processor is the whole point of it.
    const auto applyGain = [&](size_t buffer)
    {
      const float gain = DbToLinear(block.settings.gainDb);
      if (gain == 1.0f)
        return;
      for (unsigned int i = 0; i < frames; i++)
        mScratch[buffer][i] *= static_cast<NAM_SAMPLE>(gain);
    };

    if (blockOn && block.settings.type == BlockType::Nam)
    {
      const auto& model = block.namModel;
      if (model && model->NumInputChannels() == 1 && model->NumOutputChannels() == 1)
      {
        applyGain(result);
        model->process(mScratchPtrs[result].data(), mScratchPtrs[tmp].data(), static_cast<int>(frames));
        const size_t dry = result;
        result = tmp;
        ran = true;

        // The buffer we read from still holds this block's untouched input, and nothing downstream
        // needs it any more, so the block's own dry mix costs no extra memory.
        if (blend > 0.0f)
        {
          const NAM_SAMPLE wetGain = static_cast<NAM_SAMPLE>(1.0f - blend);
          const NAM_SAMPLE dryGain = static_cast<NAM_SAMPLE>(blend);
          for (unsigned int i = 0; i < frames; i++)
            mScratch[result][i] = mScratch[result][i] * wetGain + mScratch[dry][i] * dryGain;
        }
      }
    }
    else if (blockOn && block.settings.type == BlockType::Eq)
    {
      applyGain(result);

      if (blend > 0.0f)
        std::copy(mScratch[result].begin(), mScratch[result].begin() + frames, mScratch[tmp].begin());

      ParametricEq& peq = mPeqByPosition[index];
      peq.ApplyIfChanged(block.settings.peq);
      runInDoubleOn(result, [&](double** in, size_t n) { return peq.Process(in, n); });
      ran = true;

      if (blend > 0.0f)
      {
        const NAM_SAMPLE wetGain = static_cast<NAM_SAMPLE>(1.0f - blend);
        const NAM_SAMPLE dryGain = static_cast<NAM_SAMPLE>(blend);
        for (unsigned int i = 0; i < frames; i++)
          mScratch[result][i] = mScratch[result][i] * wetGain + mScratch[tmp][i] * dryGain;
      }
    }
    else if (blockOn && block.settings.type == BlockType::Comp)
    {
      applyGain(result);

      if (blend > 0.0f)
        std::copy(mScratch[result].begin(), mScratch[result].begin() + frames, mScratch[tmp].begin());

      Compressor& comp = mCompByPosition[index];
      comp.SetParams(block.settings.compPeak, block.settings.compLimit);
      comp.Process(mScratch[result].data(), frames);
      mBlockGainReduction[index].store(comp.GetGainReductionDb(), std::memory_order_relaxed);
      ran = true;

      if (blend > 0.0f)
      {
        const NAM_SAMPLE wetGain = static_cast<NAM_SAMPLE>(1.0f - blend);
        const NAM_SAMPLE dryGain = static_cast<NAM_SAMPLE>(blend);
        for (unsigned int i = 0; i < frames; i++)
          mScratch[result][i] = mScratch[result][i] * wetGain + mScratch[tmp][i] * dryGain;
      }
    }
    else if (blockOn && block.settings.type == BlockType::Cut)
    {
      applyGain(result);

      // Filtering happens in place, so the dry copy is taken first - same as the IR path below.
      if (blend > 0.0f)
        std::copy(mScratch[result].begin(), mScratch[result].begin() + frames, mScratch[tmp].begin());

      CutFilters& cut = mCutByPosition[index];
      const float lowHz = block.settings.lowCutHz;
      const float highHz = block.settings.highCutHz;

      // At the far end of its travel a cut does nothing at all, rather than sitting at the edge of
      // the audible band still costing two filter passes.
      if (lowHz > kLowCutMinHz)
      {
        if (lowHz != cut.appliedLowHz)
        {
          for (auto& section : cut.low)
            section.SetParams(recursive_linear_filter::HighPassParams(mStreamSampleRate, static_cast<double>(lowHz)));
          cut.appliedLowHz = lowHz;
        }
        for (auto& section : cut.low)
          runInDoubleOn(result, [&](double** in, size_t n) { return section.Process(in, 1, n)[0]; });
      }

      if (highHz < kHighCutMaxHz)
      {
        if (highHz != cut.appliedHighHz)
        {
          for (auto& section : cut.high)
            section.SetParams(recursive_linear_filter::LowPassParams(mStreamSampleRate, static_cast<double>(highHz)));
          cut.appliedHighHz = highHz;
        }
        for (auto& section : cut.high)
          runInDoubleOn(result, [&](double** in, size_t n) { return section.Process(in, 1, n)[0]; });
      }

      ran = true;

      if (blend > 0.0f)
      {
        const NAM_SAMPLE wetGain = static_cast<NAM_SAMPLE>(1.0f - blend);
        const NAM_SAMPLE dryGain = static_cast<NAM_SAMPLE>(blend);
        for (unsigned int i = 0; i < frames; i++)
          mScratch[result][i] = mScratch[result][i] * wetGain + mScratch[tmp][i] * dryGain;
      }
    }
    else if (blockOn && block.settings.type == BlockType::Ir && block.ir)
    {
      applyGain(result);

      // The IR owns its buffers and works in place here, so the dry copy is taken beforehand.
      if (blend > 0.0f)
        std::copy(mScratch[result].begin(), mScratch[result].begin() + frames, mScratch[tmp].begin());

      const auto& ir = block.ir;
      runInDoubleOn(result, [&](double** in, size_t n) { return ir->Process(in, 1, n)[0]; });
      ran = true;

      if (blend > 0.0f)
      {
        const NAM_SAMPLE wetGain = static_cast<NAM_SAMPLE>(1.0f - blend);
        const NAM_SAMPLE dryGain = static_cast<NAM_SAMPLE>(blend);
        for (unsigned int i = 0; i < frames; i++)
          mScratch[result][i] = mScratch[result][i] * wetGain + mScratch[tmp][i] * dryGain;
      }
    }

    if (ran)
    {
      const float level = DbToLinear(block.settings.levelDb);
      if (level != 1.0f)
        for (unsigned int i = 0; i < frames; i++)
          mScratch[result][i] *= static_cast<NAM_SAMPLE>(level);
    }

    if (runEq && eq.placement == EqPlacement::Post)
      applyEq(result);

    // Metered whether or not the block ran, so a disabled one reads what passes through it.
    float blockPeak = 0.0f;
    for (unsigned int i = 0; i < frames; i++)
      blockPeak = std::max(blockPeak, std::fabs(static_cast<float>(mScratch[result][i])));
    mBlockLevels[index].store(LinearToDb(blockPeak), std::memory_order_relaxed);

    return result;
  };

  // A run of blocks on one buffer pair. rowFilter of -1 takes every block, otherwise only those
  // on that branch.
  const auto runRange = [&](size_t from, size_t to, int rowFilter, size_t src, size_t tmp) -> size_t
  {
    size_t result = src;
    size_t spare = tmp;
    for (size_t index = from; index < to && index < mChainSnapshot.size() && index < kMaxBlocks; index++)
    {
      if (rowFilter >= 0 && mChainSnapshot[index].settings.row != rowFilter)
        continue;
      const size_t next = runOneBlock(index, result, spare);
      if (next != result)
        spare = result; // the pair swapped; the old buffer becomes the working one
      result = next;
    }
    return result;
  };

  const size_t blockCount = std::min(mChainSnapshot.size(), kMaxBlocks);
  const ChainRouting routing = GetRouting();

  // Buffer 2 carries the lower branch, buffer 3 parks the upper branch's result while the lower
  // one runs. The two free buffers of {0,1} are the working pair for each branch in turn.
  constexpr size_t kLowerBuffer = 2;
  constexpr size_t kUpperHold = 3;

  // Walk the chain section by section: series up to each split, two branches through it, series
  // again after the merge. A chain with no sections falls straight through to the tail.
  size_t cursor = 0;
  const size_t sectionCount = bypassed ? 0 : std::min(routing.sectionCount, kMaxParallelSections);

  for (size_t s = 0; s < sectionCount; s++)
  {
    const ParallelSection& section = routing.sections[s];
    const size_t splitAt = std::min(section.splitIndex, blockCount);
    const size_t mergeAt = std::min(section.mergeIndex, blockCount);
    if (mergeAt <= splitAt || splitAt < cursor)
      continue;

    current = runRange(cursor, splitAt, -1, current, current == 0 ? 1 : 0);

    std::copy(mScratch[current].begin(), mScratch[current].begin() + frames, mScratch[kLowerBuffer].begin());

    if (section.mode == SplitMode::Crossover)
    {
      if (section.crossoverHz != mAppliedCrossoverHz[s])
      {
        mCrossoverLow[s].SetParams(
          recursive_linear_filter::LowPassParams(mStreamSampleRate, static_cast<double>(section.crossoverHz)));
        mCrossoverHigh[s].SetParams(
          recursive_linear_filter::HighPassParams(mStreamSampleRate, static_cast<double>(section.crossoverHz)));
        mAppliedCrossoverHz[s] = section.crossoverHz;
      }
      runInDoubleOn(current, [&](double** in, size_t n) { return mCrossoverLow[s].Process(in, 1, n)[0]; });
      runInDoubleOn(kLowerBuffer, [&](double** in, size_t n) { return mCrossoverHigh[s].Process(in, 1, n)[0]; });
    }

    const size_t upperResult = runRange(splitAt, mergeAt, 0, current, current == 0 ? 1 : 0);
    std::copy(mScratch[upperResult].begin(), mScratch[upperResult].begin() + frames, mScratch[kUpperHold].begin());

    const size_t lowerResult = runRange(splitAt, mergeAt, 1, kLowerBuffer, 0);

    // Each branch has its own level, so moving one leaves the other exactly where it was.
    const float mergeGain = DbToLinear(section.mergeLevelDb);
    const NAM_SAMPLE upperGain = static_cast<NAM_SAMPLE>(DbToLinear(section.upperDb) * mergeGain);
    const NAM_SAMPLE lowerGain = static_cast<NAM_SAMPLE>(DbToLinear(section.lowerDb) * mergeGain);
    for (unsigned int i = 0; i < frames; i++)
      mScratch[lowerResult][i] = mScratch[kUpperHold][i] * upperGain + mScratch[lowerResult][i] * lowerGain;

    // Back onto buffer 0, so the next section can count on {0,1} being its working pair and on
    // 2 and 3 being free for its branches.
    if (lowerResult != 0)
      std::copy(mScratch[lowerResult].begin(), mScratch[lowerResult].begin() + frames, mScratch[0].begin());
    current = 0;
    cursor = mergeAt;
  }

  // Whatever is left after the last merge - or the whole chain, if there are no sections.
  current = runRange(cursor, blockCount, -1, current, current == 0 ? 1 : 0);

  if (gateOn && gate.placement == GatePlacement::Post)
    applyGate();

  const float muteGain = mMuted.load(std::memory_order_relaxed) ? 0.0f : 1.0f;

  // The chain's own level first, then the click on top of it. Mixed after the output gain so the
  // metronome holds its level while you set your playing level - but still inside the mute, since
  // a click during silent tuning helps nobody.
  float outputPeak = 0.0f;
  float chainPeak = 0.0f;
  for (unsigned int i = 0; i < frames; i++)
  {
    const float sample = static_cast<float>(mScratch[current][i]) * outputGain;
    mScratch[current][i] = static_cast<NAM_SAMPLE>(sample);
    chainPeak = std::max(chainPeak, std::fabs(sample));
  }

  // Taken before the click is mixed in, and kept apart from the output level for that reason: a
  // metronome tick is a loud, short transient, and anything watching for the chain running out of
  // headroom would otherwise be tripped by it every beat.
  mChainLevelDb.store(LinearToDb(chainPeak), std::memory_order_relaxed);

  {
    MetronomeSettings metronome;
    {
      std::lock_guard<std::mutex> lock(mMetronomeMutex);
      metronome = mMetronomeSettings;
    }
    mMetronome.SetSettings(metronome);
    mMetronome.Process(mScratch[current].data(), frames);
  }

  for (unsigned int i = 0; i < frames; i++)
  {
    const float sample = static_cast<float>(mScratch[current][i]) * muteGain;
    outputPeak = std::max(outputPeak, std::fabs(sample));
    output[i * 2 + 0] = sample;
    output[i * 2 + 1] = sample;
  }

  // The backing track, added on top of what the chain produced. It is stereo where the chain is
  // mono, which is the other reason it is mixed here rather than fed through the blocks.
  if (muteGain > 0.0f)
    mPlayer.Process(output, frames);
  for (unsigned int i = frames; i < nFrames; i++)
  {
    output[i * 2 + 0] = 0.0f;
    output[i * 2 + 1] = 0.0f;
  }

  mOutputLevelDb.store(LinearToDb(outputPeak), std::memory_order_relaxed);
  return 0;
}

} // namespace nam_ui
