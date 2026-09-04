#include "CaptureLibrary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>

#include "json.hpp"

#include "NAM/get_dsp.h"

namespace nam_ui
{

namespace
{

std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

/// Adds a value to a list if it isn't already there and isn't empty.
void InsertUnique(std::vector<std::string>& values, const std::string& value)
{
  if (value.empty())
    return;
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

std::vector<std::string> JsonStringArray(const nlohmann::json& j, const char* key)
{
  std::vector<std::string> out;
  if (j.contains(key) && j[key].is_array())
    for (const auto& entry : j[key])
      if (entry.is_string())
        out.push_back(entry.get<std::string>());
  return out;
}

} // namespace

CaptureMetadata CaptureMetadata::Load(const std::filesystem::path& folder)
{
  CaptureMetadata meta;

  std::ifstream in(folder / kFileName);
  if (!in.is_open())
    return meta;

  try
  {
    nlohmann::json j;
    in >> j;
    meta.title = j.value("title", std::string());
    meta.gear = j.value("gear", std::string());
    meta.creator = j.value("creator", std::string());
    meta.architecture = j.value("architecture", std::string());
    meta.license = j.value("license", std::string());
    meta.tags = JsonStringArray(j, "tags");
    meta.makes = JsonStringArray(j, "makes");
    meta.known = true;
  }
  catch (const std::exception&)
  {
    return CaptureMetadata(); // a broken sidecar just means "no metadata", never a failed scan
  }

  return meta;
}

void CaptureMetadata::Save(const std::filesystem::path& folder) const
{
  nlohmann::json j;
  j["title"] = title;
  j["gear"] = gear;
  j["creator"] = creator;
  j["architecture"] = architecture;
  j["license"] = license;
  j["tags"] = tags;
  j["makes"] = makes;

  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::ofstream out(folder / kFileName);
  if (out.is_open())
    out << j.dump(2);
}

CaptureLibrary::CaptureLibrary()
{
  mWorker = std::thread(&CaptureLibrary::WorkerLoop, this);
}

CaptureLibrary::~CaptureLibrary()
{
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mStopping = true;
  }
  mWakeWorker.notify_all();
  if (mWorker.joinable())
    mWorker.join();
}

void CaptureLibrary::SetFolder(std::filesystem::path folder)
{
  mFolder = std::move(folder);
  Refresh();
}

void CaptureLibrary::Refresh()
{
  mEntries.clear();
  mKnownTags.clear();
  mKnownGear.clear();
  mKnownMakes.clear();
  mKnownCreators.clear();
  if (mFolder.empty())
    return;

  std::error_code ec;
  if (!std::filesystem::is_directory(mFolder, ec))
    return;

  // One sidecar serves every file in its folder, so read each folder's metadata only once.
  std::map<std::filesystem::path, CaptureMetadata> metadataByFolder;

  // Recursive: downloads are filed into per-tone subfolders.
  for (auto it = std::filesystem::recursive_directory_iterator(
         mFolder, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
  {
    if (ec)
      break;
    if (!it->is_regular_file(ec))
      continue;

    // .nam are captures for NAM blocks; .wav are cab impulse responses for IR blocks.
    const std::string ext = ToLower(it->path().extension().string());
    const bool isIr = (ext == ".wav");
    if (ext != ".nam" && !isIr)
      continue;

    const auto parent = it->path().parent_path();
    auto found = metadataByFolder.find(parent);
    if (found == metadataByFolder.end())
      found = metadataByFolder.emplace(parent, CaptureMetadata::Load(parent)).first;

    CaptureEntry entry;
    entry.path = it->path();
    entry.displayName = it->path().stem().string();
    entry.isIr = isIr;
    entry.meta = found->second;
    if (parent != mFolder)
      entry.groupName = parent.filename().string();

    mEntries.push_back(std::move(entry));
  }

  std::sort(mEntries.begin(), mEntries.end(),
            [](const CaptureEntry& a, const CaptureEntry& b)
            {
              if (a.groupName != b.groupName)
                return a.groupName < b.groupName;
              return a.displayName < b.displayName;
            });

  RebuildDerived();
}

void CaptureLibrary::SetUserTags(std::map<std::string, std::vector<std::string>> tags)
{
  mUserTags = std::move(tags);
  RebuildDerived();
}

void CaptureLibrary::RebuildDerived()
{
  mKnownTags.clear();
  mKnownGear.clear();
  mKnownMakes.clear();
  mKnownCreators.clear();

  for (auto& entry : mEntries)
  {
    const auto found = mUserTags.find(entry.path.string());
    entry.userTags = (found != mUserTags.end()) ? found->second : std::vector<std::string>();

    // Build the search text once: name, folder, and every piece of metadata worth matching.
    std::string haystack = entry.displayName + " " + entry.groupName + " " + entry.meta.title + " "
                           + entry.meta.gear + " " + entry.meta.creator;
    for (const auto& tag : entry.meta.tags)
      haystack += " " + tag;
    for (const auto& tag : entry.userTags)
      haystack += " " + tag;
    for (const auto& make : entry.meta.makes)
      haystack += " " + make;
    entry.searchHaystack = ToLower(std::move(haystack));

    // A tag you added filters exactly like one that came with the capture; the filter list has no
    // reason to care which is which.
    for (const auto& tag : entry.meta.tags)
      InsertUnique(mKnownTags, tag);
    for (const auto& tag : entry.userTags)
      InsertUnique(mKnownTags, tag);
    for (const auto& make : entry.meta.makes)
      InsertUnique(mKnownMakes, make);
    InsertUnique(mKnownGear, entry.meta.gear);
    InsertUnique(mKnownCreators, entry.meta.creator);
  }

  std::sort(mKnownTags.begin(), mKnownTags.end());
  std::sort(mKnownGear.begin(), mKnownGear.end());
  std::sort(mKnownMakes.begin(), mKnownMakes.end());
  std::sort(mKnownCreators.begin(), mKnownCreators.end());
}

void CaptureLibrary::RequestLoad(int blockId, BlockType type, const std::filesystem::path& path, double sampleRate,
                                 int maxBufferSize)
{
  {
    std::lock_guard<std::mutex> lock(mMutex);

    // Only the newest request for a block matters; drop any earlier one still queued so holding
    // an arrow key down does not build up a backlog of loads nobody will hear.
    mPending.erase(std::remove_if(mPending.begin(), mPending.end(),
                                  [blockId](const LoadRequest& request) { return request.blockId == blockId; }),
                   mPending.end());

    LoadRequest request;
    request.blockId = blockId;
    request.type = type;
    request.path = path;
    request.sampleRate = sampleRate;
    request.maxBufferSize = maxBufferSize;
    mPending.push_back(std::move(request));
    mInFlight.insert(blockId);
  }
  mWakeWorker.notify_one();
}

bool CaptureLibrary::PopCompleted(LoadResult& out)
{
  std::lock_guard<std::mutex> lock(mMutex);
  if (mCompleted.empty())
    return false;
  out = std::move(mCompleted.front());
  mCompleted.pop_front();
  return true;
}

bool CaptureLibrary::IsLoading(int blockId) const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mInFlight.find(blockId) != mInFlight.end();
}

void CaptureLibrary::CancelLoads(int blockId)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mPending.erase(std::remove_if(mPending.begin(), mPending.end(),
                                [blockId](const LoadRequest& request) { return request.blockId == blockId; }),
                 mPending.end());
  mCompleted.erase(std::remove_if(mCompleted.begin(), mCompleted.end(),
                                  [blockId](const LoadResult& result) { return result.blockId == blockId; }),
                   mCompleted.end());
  mInFlight.erase(blockId);
}

void CaptureLibrary::WorkerLoop()
{
  for (;;)
  {
    LoadRequest request;
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mWakeWorker.wait(lock, [this] { return mStopping || !mPending.empty(); });
      if (mStopping)
        return;
      request = std::move(mPending.front());
      mPending.pop_front();
    }

    LoadResult result;
    result.blockId = request.blockId;
    result.type = request.type;
    result.path = request.path;

    try
    {
      if (request.type == BlockType::Nam)
      {
        auto model = nam::get_dsp(request.path);
        if (!model)
          throw std::runtime_error("Model failed to load (unsupported or corrupt file).");
        // Reset here, on the worker, so handing this to the engine later costs nothing.
        model->Reset(request.sampleRate, request.maxBufferSize);
        result.namModel = std::move(model);
      }
      else
      {
        // The constructor reads the WAV and resamples it to the stream rate, so a 44.1 kHz IR
        // works in a 48 kHz rig without the caller doing anything.
        auto ir = std::make_shared<dsp::ImpulseResponse>(request.path.string().c_str(), request.sampleRate);
        const dsp::wav::LoadReturnCode wavState = ir->GetWavState();
        if (wavState != dsp::wav::LoadReturnCode::SUCCESS)
          throw std::runtime_error("Could not load IR: " + dsp::wav::GetMsgForLoadReturnCode(wavState));
        result.ir = std::move(ir);
      }
    }
    catch (const std::exception& e)
    {
      result.namModel.reset();
      result.ir.reset();
      result.error = e.what();
    }

    {
      std::lock_guard<std::mutex> lock(mMutex);
      // Another request for this block may have arrived while we worked; leave it marked busy.
      const bool stillQueued = std::any_of(mPending.begin(), mPending.end(), [&](const LoadRequest& pending)
                                           { return pending.blockId == request.blockId; });
      if (!stillQueued)
        mInFlight.erase(request.blockId);
      mCompleted.push_back(std::move(result));
    }
  }
}

} // namespace nam_ui
