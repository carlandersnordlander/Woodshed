#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "RtAudio.h"

#include "Compressor.h"
#include "ImpulseResponse.h"
#include "NAM/dsp.h"
#include "Metronome.h"
#include "NoiseGate.h"
#include "Player.h"
#include "ParametricEq.h"
#include "StageEq.h"
#include "Tuner.h"

namespace nam_ui
{

// Upper bound on the block size the audio callback will ever be asked to process. Scratch
// buffers are sized to this once, up front, so the (real-time) audio thread never allocates.
constexpr unsigned int kMaxBlockFrames = 8192;

/// How many units the chain can hold. Fixed so the audio thread's snapshot buffer and the
/// per-position filter state can both be allocated once, at startup - which is the only reason
/// there is a limit at all. Eight was a number that ran out while building an ordinary rig; the
/// cost of the ones nobody uses is a few filter structs sitting idle.
constexpr size_t kMaxBlocks = 24;

/// What a block does. Everything else about a block - enable, trim, blend, EQ - is the same
/// either way, which is the point of treating them uniformly.
enum class BlockType
{
  Nam, ///< a .nam capture
  Ir, ///< a cab impulse response
  Cut, ///< a low cut and a high cut, nothing loaded into it
  Comp, ///< an optical-style compressor
  Eq ///< a five band parametric EQ with a spectrum display
};

// A cut is off at the end of its travel rather than behind a switch: dragging a low cut all the
// way left leaves everything alone, and the same for a high cut dragged all the way right.
// Both run the whole audible band, so the two can be brought together into a narrow band anywhere
// you like rather than each being confined to its own half.
constexpr float kLowCutMinHz = 20.0f; ///< at or below this the low cut is off
constexpr float kLowCutMaxHz = 20000.0f;
constexpr float kHighCutMinHz = 20.0f;
constexpr float kHighCutMaxHz = 20000.0f; ///< at or above this the high cut is off

/// Everything about a block except what is loaded into it.
struct BlockSettings
{
  BlockType type = BlockType::Nam;
  bool enabled = true;
  /// How hard the block is driven, applied before its processor. Captures are level-sensitive, so
  /// this is as much a tone control as a level one: more going in is more break-up coming out.
  float gainDb = 0.0f;
  /// The block's own volume, applied after its processor - what the gain above is compensated with.
  float levelDb = 0.0f;
  /// Parallel clean path around this one block, 0..1. Separate from the chain's parallel branches.
  float dryBlend = 0.0f;
  /// Cut blocks only. Both start at the end of their travel that does nothing.
  float lowCutHz = kLowCutMinHz;
  float highCutHz = kHighCutMaxHz;

  /// Compressor blocks only. How hard it works, 0-10, and which of the two ratios it uses.
  /// Makeup gain is the block's own volume and the wet/dry mix is its blend - a compressor needs
  /// both, and the block already has them.
  float compPeak = 5.0f;
  bool compLimit = false;

  /// EQ blocks only.
  ParametricEqSettings peq;
  /// Which branch this block sits on inside the parallel region: 0 = upper, 1 = lower.
  /// Ignored for blocks outside that region, which are always serial.
  int row = 0;
  EqSettings eq;
};

/// How the signal is divided when it splits into two branches.
enum class SplitMode
{
  Full, ///< both branches get the whole signal; the merge decides the balance
  Crossover ///< low frequencies to the upper branch, highs to the lower
};

/// How many parallel sections a chain can hold. Each one needs a block on each of its branches,
/// so the block limit caps this anyway; stating it lets the routing be a fixed-size struct.
constexpr size_t kMaxParallelSections = kMaxBlocks / 2;

/// "No section" - what ChainRouting::SectionAt returns for a block on the main line.
constexpr size_t kNoSection = static_cast<size_t>(-1);

/// A parallel section of the chain: everything in [splitIndex, mergeIndex) runs on two branches
/// at once, chosen per block by its row, and the two are mixed back together afterwards.
struct ParallelSection
{
  size_t splitIndex = 0;
  size_t mergeIndex = 0;
  SplitMode mode = SplitMode::Full;
  float crossoverHz = 500.0f;
  /// \brief How loud each branch is at the merge, in dB - two independent levels, not a balance.
  ///
  /// A single mix control can only say how the two compare: turning one up turns the other down,
  /// and the number it shows says nothing about how loud either branch actually is. Two levels are
  /// what a mixer would give you, and what a merge is - two things being summed.
  float upperDb = 0.0f;
  float lowerDb = 0.0f;
  /// The old single balance, kept only so a config or rig written before the two levels existed
  /// still loads. Converted on the way in and never used afterwards.
  float mix = 0.5f;
  /// Level of the merged signal. Summing two branches usually changes the level, so the merge
  /// needs its own trim to put the chain back where it was.
  float mergeLevelDb = 0.0f;
};

/// Where the chain splits and rejoins. Any number of sections, in order and never overlapping;
/// none at all means plain series.
///
/// Fixed storage rather than a vector: the audio thread takes a copy of this every block, and a
/// copy that allocates is a copy that can stall.
struct ChainRouting
{
  std::array<ParallelSection, kMaxParallelSections> sections{};
  size_t sectionCount = 0;

  /// Which section chain position `blockIndex` sits in, or kNoSection if it is on the main line.
  size_t SectionAt(size_t blockIndex) const;
};

/// One unit in the chain, as the engine holds it.
struct Block
{
  int id = 0; ///< stable across reordering, so the UI and loader can refer to a block
  BlockSettings settings;
  std::shared_ptr<nam::DSP> namModel;
  std::shared_ptr<dsp::ImpulseResponse> ir;
};

/// Where the gate's attenuation lands. Detection always listens to the clean input either way -
/// a gate triggered off a distorted signal chatters and its threshold stops relating to how hard
/// you actually played.
enum class GatePlacement
{
  Pre = 0, ///< attenuate ahead of the chain, so nothing downstream amplifies the noise
  Post = 1 ///< attenuate at the very end, catching hiss the chain itself adds
};

struct GateSettings
{
  bool enabled = false;
  GatePlacement placement = GatePlacement::Pre;
  float thresholdDb = -60.0f;
};

struct AudioDeviceChoice
{
  RtAudio::Api api = RtAudio::UNSPECIFIED;
  unsigned int inputDeviceId = 0;
  unsigned int outputDeviceId = 0;
  /// Which socket on the interface, not just which interface. An eight-input box has one device
  /// and eight places to plug a guitar into, and picking the box says nothing about which of them
  /// is being listened to. The input is mono, so this is the one channel; the output is a stereo
  /// pair, so it is the first of the two.
  unsigned int inputFirstChannel = 0;
  unsigned int outputFirstChannel = 0;
  unsigned int sampleRate = 48000;
  unsigned int bufferFrames = 256;
};

/// Owns the RtAudio stream and the block chain. The chain is guarded by a mutex the audio thread
/// holds only long enough to copy a snapshot of it, never for the processing itself, so editing
/// the chain from the UI never blocks audio for meaningfully long.
class AudioEngine
{
public:
  AudioEngine() = default;
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  std::vector<RtAudio::Api> GetCompiledApis() const;
  /// Also captures any warning RtAudio emits while probing, retrievable via
  /// GetLastProbeMessage() - useful when the returned list comes back empty.
  std::vector<RtAudio::DeviceInfo> GetDevices(RtAudio::Api api) const;
  const std::string& GetLastProbeMessage() const { return mLastProbeMessage; }

  /// Opens (reopening if already open) the stream. Empty string on success, else an error message.
  std::string Open(const AudioDeviceChoice& choice);
  void Close();
  bool IsOpen() const { return mIsOpen; }

  // --- the chain -------------------------------------------------------------------------

  /// Appends a block and returns its id, or -1 if the chain is already at kMaxBlocks.
  int AddBlock(BlockType type);
  void RemoveBlock(int id);
  /// Moves a block one position earlier or later in the chain.
  void MoveBlock(int id, int delta);
  /// Reorders so that the block with `id` sits at `newIndex`, shifting the rest along.
  void ReorderBlock(int id, size_t newIndex);

  size_t GetBlockCount() const;
  /// A copy, so the caller is never holding the lock while it draws.
  std::vector<Block> GetBlocks() const;
  bool GetBlockSettings(int id, BlockSettings& out) const;
  void SetBlockSettings(int id, const BlockSettings& settings);

  /// Moves a block between the branches of a parallel section. Sending one to the lower branch is
  /// what creates a section, or grows the one next to it; this is where that policy lives, so the
  /// row and the routing can never disagree with each other.
  void SetBlockRow(int id, int row);

  /// Grows the section around `id` until it also covers `otherId`, as far as the neighbouring
  /// sections allow. Dropping a block opposite another one means running the two alongside each
  /// other, and that only holds if both are inside the same section.
  void ExtendSectionOver(int id, int otherId);

  /// The processor must already be prepared for the engine's current stream settings.
  void SetBlockNamModel(int id, std::shared_ptr<nam::DSP> model);
  void SetBlockIr(int id, std::shared_ptr<dsp::ImpulseResponse> ir);
  void ClearBlockProcessor(int id);

  /// Peak level leaving the block at `index`, for the per-block meters.
  float GetBlockLevelDb(size_t index) const;
  /// Gain reduction a compressor block is applying, in dB and positive. Zero for other types.
  float GetBlockGainReductionDb(size_t index) const;

  /// What is going into the block at `index`, for an EQ block to draw a spectrum of. Only the
  /// selected block is tapped, since nothing else has anywhere to show it.
  void SetSpectrumBlock(int id) { mSpectrumBlockId.store(id, std::memory_order_relaxed); }
  const SpectrumTap& GetSpectrum() const { return mSpectrum; }

  ChainRouting GetRouting() const;
  void SetRouting(const ChainRouting& routing);

  // --- everything else -------------------------------------------------------------------

  void SetGate(const GateSettings& settings);
  GateSettings GetGate() const;

  /// The tuner listens to the clean input, ahead of everything else: distortion adds harmonics
  /// and smears the phase the strobe depends on.
  /// The metronome sits alongside the chain rather than in it: a click through an amp capture is
  /// a distorted click, and its level should not move when you set your playing level.
  void SetMetronome(const MetronomeSettings& settings);
  MetronomeSettings GetMetronome() const;
  /// Places the click where a song already in progress is, rather than restarting the count.
  /// Safe from any thread; the audio thread picks it up on its next block.
  void SyncMetronome(double fractionIntoBeat, int beatIndex) { mMetronome.RequestSync(fractionIntoBeat, beatIndex); }
  float GetMetronomePhase() const { return mMetronome.GetPhase(); }
  int GetMetronomeBeat() const { return mMetronome.GetBeat(); }
  int GetMetronomeBar() const { return mMetronome.GetBar(); }

  /// The backing-track player. Alongside the chain, not through it: a track run through an amp
  /// capture is a track played through an amp, which is not what anyone wants to practise against.
  Player& GetPlayer() { return mPlayer; }

  Tuner& GetTuner() { return mTuner; }
  void SetTunerEnabled(bool enabled) { mTunerEnabled.store(enabled, std::memory_order_relaxed); }
  bool IsTunerEnabled() const { return mTunerEnabled.load(std::memory_order_relaxed); }

  /// Silences the output without touching the chain, so tuning stays private.
  void SetMuted(bool muted) { mMuted.store(muted, std::memory_order_relaxed); }
  bool IsMuted() const { return mMuted.load(std::memory_order_relaxed); }

  void SetInputGainDb(float db) { mInputGainDb.store(db, std::memory_order_relaxed); }
  void SetOutputGainDb(float db) { mOutputGainDb.store(db, std::memory_order_relaxed); }
  float GetInputGainDb() const { return mInputGainDb.load(std::memory_order_relaxed); }
  float GetOutputGainDb() const { return mOutputGainDb.load(std::memory_order_relaxed); }

  void SetBypassed(bool bypassed) { mBypassed.store(bypassed, std::memory_order_relaxed); }
  bool IsBypassed() const { return mBypassed.load(std::memory_order_relaxed); }

  float GetInputLevelDb() const { return mInputLevelDb.load(std::memory_order_relaxed); }
  float GetOutputLevelDb() const { return mOutputLevelDb.load(std::memory_order_relaxed); }
  /// What the chain itself is putting out, before the click is mixed on top. The output level is
  /// everything that leaves the card, which is the right thing for a meter and the wrong thing for
  /// anything watching the chain for clipping: a metronome tick would trip it every beat.
  float GetChainLevelDb() const { return mChainLevelDb.load(std::memory_order_relaxed); }

  unsigned int GetActualSampleRate() const { return mActiveChoice.sampleRate; }
  unsigned int GetActualBufferFrames() const { return mActiveChoice.bufferFrames; }
  const std::string& GetLastError() const { return mLastError; }
  int GetUnderrunCount() const { return mUnderrunCount.load(std::memory_order_relaxed); }

private:
  static int AudioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames, double streamTime,
                           RtAudioStreamStatus status, void* userData);
  int Process(float* output, const float* input, unsigned int nFrames, RtAudioStreamStatus status);

  /// Index of the block with this id, or npos. Caller holds mChainMutex.
  size_t FindBlock(int id) const;

  /// Brings the routing back into agreement with the chain: indices in range, and no parallel
  /// section left standing once nothing is on its lower branch. Caller holds mChainMutex.
  void ValidateRoutingLocked();

  std::unique_ptr<RtAudio> mRtAudio;
  AudioDeviceChoice mActiveChoice;
  bool mIsOpen = false;
  std::string mLastError;
  mutable std::string mLastProbeMessage;

  mutable std::mutex mChainMutex;
  std::vector<Block> mChain;
  ChainRouting mRouting;
  int mNextBlockId = 1;

  // Crossover for a frequency split, one pair per section since each has its own frequency and
  // its own filter state. One-pole sections rather than a Linkwitz-Riley pair, because that is
  // what AudioDSPTools offers; gentle, and adequate for splitting a rig in two.
  std::array<recursive_linear_filter::LowPass, kMaxParallelSections> mCrossoverLow; ///< audio-thread only
  std::array<recursive_linear_filter::HighPass, kMaxParallelSections> mCrossoverHigh; ///< audio-thread only
  std::array<float, kMaxParallelSections> mAppliedCrossoverHz{}; ///< audio-thread only
  double mStreamSampleRate = 48000.0;

  /// The audio thread's copy of the chain, filled under the lock each block. Its capacity is
  /// reserved up front so taking the snapshot never allocates.
  std::vector<Block> mChainSnapshot;

  /// One cut block's filters. Two one-pole sections per cut, cascaded: AudioDSPTools offers only
  /// single-pole filters, and 6 dB per octave is too gentle to be worth calling a cut.
  struct CutFilters
  {
    recursive_linear_filter::HighPass low[2];
    recursive_linear_filter::LowPass high[2];
    float appliedLowHz = -1.0f;
    float appliedHighHz = -1.0f;
  };

  /// EQ filter state belongs to a chain position rather than to a block: reordering then costs a
  /// few milliseconds of filter re-convergence instead of any bookkeeping. The cuts follow the
  /// same rule for the same reason.
  std::array<StageEq, kMaxBlocks> mEqByPosition;
  std::array<CutFilters, kMaxBlocks> mCutByPosition;
  std::array<Compressor, kMaxBlocks> mCompByPosition;
  std::array<ParametricEq, kMaxBlocks> mPeqByPosition;
  std::array<std::atomic<float>, kMaxBlocks> mBlockLevels;
  /// How much a compressor block is pulling down, so the panel can show it working.
  std::array<std::atomic<float>, kMaxBlocks> mBlockGainReduction;

  // The gate's trigger analyses the signal and pushes gain reduction to its Gain listener, which
  // is what lets detection sit on the input while the attenuation lands wherever the user wants.
  dsp::noise_gate::Trigger mGateTrigger; ///< audio-thread only
  dsp::noise_gate::Gain mGateGain; ///< audio-thread only
  std::atomic<bool> mGateEnabled{false};
  std::atomic<int> mGatePlacement{static_cast<int>(GatePlacement::Pre)};
  std::atomic<float> mGateThresholdDb{-60.0f};
  float mAppliedGateThresholdDb = 0.0f; ///< audio-thread only
  bool mGateParamsApplied = false; ///< audio-thread only

  /// The signal entering whichever block asked to be watched.
  SpectrumTap mSpectrum;
  std::atomic<int> mSpectrumBlockId{0};

  Player mPlayer;
  Metronome mMetronome; ///< audio-thread only, apart from its atomics
  mutable std::mutex mMetronomeMutex;
  MetronomeSettings mMetronomeSettings;

  Tuner mTuner;
  std::atomic<bool> mTunerEnabled{false};
  std::atomic<bool> mMuted{false};

  std::atomic<float> mInputGainDb{0.0f};
  std::atomic<float> mOutputGainDb{0.0f};
  std::atomic<bool> mBypassed{false};
  std::atomic<float> mInputLevelDb{-100.0f};
  std::atomic<float> mOutputLevelDb{-100.0f};
  std::atomic<float> mChainLevelDb{-100.0f};
  std::atomic<int> mUnderrunCount{0};

  // Audio-thread-only scratch, sized in Open(). Four float buffers: two to ping-pong a branch
  // through its blocks, one to hold the signal entering the split, and one to hold the upper
  // branch's result while the lower branch runs. Plus one double buffer for AudioDSPTools.
  std::array<std::vector<NAM_SAMPLE>, 4> mScratch;
  std::array<std::vector<NAM_SAMPLE*>, 4> mScratchPtrs;
  std::vector<double> mDoubleScratch;
  std::vector<double*> mDoubleScratchPtrs;
};

} // namespace nam_ui
