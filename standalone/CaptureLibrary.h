#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AudioEngine.h" // BlockType
#include "ImpulseResponse.h"
#include "NAM/dsp.h"

namespace nam_ui
{

/// What TONE3000 knew about a capture when it was downloaded, kept next to the .nam so the same
/// tags and categories are available for filtering long after the download.
struct CaptureMetadata
{
  std::string title;
  std::string gear;
  std::string creator;
  std::string architecture;
  std::string license;
  std::vector<std::string> tags;
  std::vector<std::string> makes;
  bool known = false;

  /// The sidecar file written beside downloaded captures, one per tone folder.
  static constexpr const char* kFileName = "_tone3000.json";

  static CaptureMetadata Load(const std::filesystem::path& folder);
  void Save(const std::filesystem::path& folder) const;
};

struct CaptureEntry
{
  std::filesystem::path path;
  std::string displayName; ///< the model file's own name
  std::string groupName; ///< the folder it sits in, when captures are organised into tone folders
  /// .wav files are cab impulse responses and load into IR blocks; .nam into NAM blocks.
  bool isIr = false;
  CaptureMetadata meta;
  /// Tags the user added to this file. Separate from meta.tags so what the capture came with stays
  /// distinguishable from what you decided about it later; both show, and both filter.
  std::vector<std::string> userTags;

  /// Everything worth matching a search box against, lowercased, built once at scan time.
  std::string searchHaystack;
};

/// A finished load, ready to be handed to the engine.
struct LoadResult
{
  int blockId = 0;
  BlockType type = BlockType::Nam;
  std::filesystem::path path;
  std::shared_ptr<nam::DSP> namModel;
  std::shared_ptr<dsp::ImpulseResponse> ir;
  std::string error;
};

/// Scans a folder tree for .nam captures and .wav impulse responses, and loads them on a worker
/// thread so neither the UI nor the audio callback ever waits on disk.
///
/// Loads are queued against a block id rather than a fixed slot, because the chain is now a list
/// the user can grow, shrink and reorder.
class CaptureLibrary
{
public:
  CaptureLibrary();
  ~CaptureLibrary();

  CaptureLibrary(const CaptureLibrary&) = delete;
  CaptureLibrary& operator=(const CaptureLibrary&) = delete;

  void SetFolder(std::filesystem::path folder);
  std::filesystem::path GetFolder() const { return mFolder; }
  void Refresh();
  const std::vector<CaptureEntry>& GetEntries() const { return mEntries; }

  /// Hands over the user's own tags, keyed by path. Re-derives the search text and the filter
  /// lists without touching the disk, so editing a tag is instant rather than a rescan.
  void SetUserTags(std::map<std::string, std::vector<std::string>> tags);

  /// Everything seen across the scanned files, sorted, to build the local filter lists from.
  const std::vector<std::string>& GetKnownTags() const { return mKnownTags; }
  const std::vector<std::string>& GetKnownGear() const { return mKnownGear; }
  const std::vector<std::string>& GetKnownMakes() const { return mKnownMakes; }
  const std::vector<std::string>& GetKnownCreators() const { return mKnownCreators; }

  /// Queue a file to be prepared for a block. NAM models are Reset() and IRs resampled on the
  /// worker, so handing the result to the engine afterwards costs nothing.
  void RequestLoad(int blockId, BlockType type, const std::filesystem::path& path, double sampleRate,
                   int maxBufferSize);

  /// Takes one finished load off the queue. Returns false when there is nothing waiting.
  bool PopCompleted(LoadResult& out);

  /// True while a load for this block is queued or running, for the UI's progress text.
  bool IsLoading(int blockId) const;

  /// Forgets any queued work for a block that has been removed.
  void CancelLoads(int blockId);

private:
  struct LoadRequest
  {
    int blockId = 0;
    BlockType type = BlockType::Nam;
    std::filesystem::path path;
    double sampleRate = 48000.0;
    int maxBufferSize = 8192;
  };

  void WorkerLoop();

  /// Recomputes what is derived from the entries rather than read off the disk: each entry's user
  /// tags and search text, and the four filter lists. Shared by Refresh and SetUserTags.
  void RebuildDerived();

  std::filesystem::path mFolder;
  std::vector<CaptureEntry> mEntries;
  std::map<std::string, std::vector<std::string>> mUserTags;
  std::vector<std::string> mKnownTags;
  std::vector<std::string> mKnownGear;
  std::vector<std::string> mKnownMakes;
  std::vector<std::string> mKnownCreators;

  mutable std::mutex mMutex;
  std::condition_variable mWakeWorker;
  std::deque<LoadRequest> mPending;
  std::deque<LoadResult> mCompleted;
  std::unordered_set<int> mInFlight;
  bool mStopping = false;
  std::thread mWorker;
};

} // namespace nam_ui
