#include "Tone3000Client.h"

#include <algorithm>
#include <fstream>

#include "json.hpp"

#include "CaptureLibrary.h" // CaptureMetadata, so the sidecar format lives in one place
#include "HttpClient.h"
#include "OAuth.h"

namespace nam_ui
{

namespace
{

constexpr const char* kApiBase = "https://www.tone3000.com/api/v1";
/// The API caps page_size at 25.
constexpr int kPageSize = 25;

std::string JsonString(const nlohmann::json& j, const char* key, const std::string& fallback = {})
{
  if (!j.contains(key) || j[key].is_null() || !j[key].is_string())
    return fallback;
  return j[key].get<std::string>();
}

int JsonInt(const nlohmann::json& j, const char* key, int fallback = 0)
{
  if (!j.contains(key) || j[key].is_null() || !j[key].is_number())
    return fallback;
  return j[key].get<int>();
}

long long JsonLongLong(const nlohmann::json& j, const char* key, long long fallback = 0)
{
  if (!j.contains(key) || j[key].is_null() || !j[key].is_number())
    return fallback;
  return j[key].get<long long>();
}

bool JsonBool(const nlohmann::json& j, const char* key, bool fallback = false)
{
  if (!j.contains(key) || j[key].is_null() || !j[key].is_boolean())
    return fallback;
  return j[key].get<bool>();
}

std::string Join(const std::vector<std::string>& values, char separator)
{
  std::string out;
  for (const auto& value : values)
  {
    if (!out.empty())
      out.push_back(separator);
    out += value;
  }
  return out;
}

const char* SortValue(SortOrder sort)
{
  switch (sort)
  {
    case SortOrder::Newest: return "newest";
    case SortOrder::Oldest: return "oldest";
    case SortOrder::Trending: return "trending";
    case SortOrder::DownloadsAllTime: return "downloads-all-time";
    case SortOrder::BestMatch:
    default: return "best-match";
  }
}

const char* ArchitectureValue(Architecture architecture)
{
  switch (architecture)
  {
    case Architecture::A2: return "2";
    case Architecture::A1: return "1";
    case Architecture::Custom: return "custom";
    case Architecture::Any:
    default: return nullptr;
  }
}

std::string SanitizeFileName(const std::string& name)
{
  std::string out;
  out.reserve(name.size());
  for (const unsigned char c : name)
  {
    if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?'
        || c == '*')
      out.push_back('_');
    else
      out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' '))
    out.pop_back();
  if (out.empty())
    out = "capture";
  return out;
}

std::string DescribeFailure(const HttpResponse& response, const char* what)
{
  if (!response.transportError.empty())
    return std::string(what) + ": " + response.transportError;

  switch (response.statusCode)
  {
    case 401:
    case 403:
      return std::string(what) + ": not authorized (status 401/403). Try connecting your TONE3000 account again.";
    case 429:
      return std::string(what) + ": rate limited by TONE3000 (status 429). Wait a moment and try again.";
    default: break;
  }

  std::string body = response.BodyAsString();
  if (body.size() > 200)
    body.resize(200);
  return std::string(what) + ": HTTP " + std::to_string(response.statusCode) + (body.empty() ? "" : " - " + body);
}

} // namespace

std::string ToneFolderName(const std::string& title)
{
  return SanitizeFileName(title);
}

AsyncSlot::~AsyncSlot()
{
  if (mWorker.joinable())
    mWorker.join();
}

void AsyncSlot::Start(std::function<void()> work)
{
  if (mBusy.load(std::memory_order_relaxed))
    return;
  if (mWorker.joinable())
    mWorker.join();

  mBusy.store(true, std::memory_order_relaxed);
  mWorker = std::thread(
    [this, work = std::move(work)]()
    {
      work();
      mBusy.store(false, std::memory_order_relaxed);
    });
}

void Tone3000Client::SetClientId(std::string clientId)
{
  // Trimmed, because these arrive by copy and paste and a trailing space or newline rides along
  // silently - the server then sees a key that is not quite the one on the screen and answers
  // "unknown client_id", which is an unhelpful thing to spend an evening on.
  const size_t first = clientId.find_first_not_of(" \t\r\n");
  const size_t last = clientId.find_last_not_of(" \t\r\n");
  clientId = (first == std::string::npos) ? std::string() : clientId.substr(first, last - first + 1);

  std::lock_guard<std::mutex> lock(mStateMutex);
  mClientId = std::move(clientId);
}

std::string Tone3000Client::GetClientId() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mClientId;
}

bool Tone3000Client::HasClientId() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return !mClientId.empty();
}

void Tone3000Client::SetRefreshToken(std::string refreshToken)
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  mRefreshToken = std::move(refreshToken);
  // No access token yet; the first request that needs one will trade the refresh token for it.
  mAccessToken.clear();
  mTokenExpiry = std::chrono::steady_clock::time_point{};
}

std::string Tone3000Client::GetRefreshToken() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mRefreshToken;
}

bool Tone3000Client::IsConnected() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return !mAccessToken.empty() || !mRefreshToken.empty();
}

void Tone3000Client::Disconnect()
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  mAccessToken.clear();
  mRefreshToken.clear();
  mTokenExpiry = std::chrono::steady_clock::time_point{};
  mStatusMessage = "Disconnected.";
}

void Tone3000Client::BeginAuthorizationAsync()
{
  if (!HasClientId())
  {
    SetStatus("Add your TONE3000 publishable key first.");
    return;
  }

  mAuthSlot.Start(
    [this]()
    {
      const PkcePair pkce = MakePkcePair();
      const std::string state = MakeRandomToken(32);
      if (pkce.verifier.empty() || state.empty())
      {
        SetStatus("Could not generate the sign-in challenge.");
        return;
      }

      const std::string clientId = [this]
      {
        std::lock_guard<std::mutex> lock(mStateMutex);
        return mClientId;
      }();

      const std::string redirectUri = OAuthRedirectUri();
      const std::string authorizeUrl = std::string(kApiBase) + "/oauth/authorize?response_type=code"
                                       + "&client_id=" + UrlEncode(clientId)
                                       + "&redirect_uri=" + UrlEncode(redirectUri)
                                       + "&code_challenge=" + UrlEncode(pkce.challenge)
                                       + "&code_challenge_method=S256" + "&state=" + UrlEncode(state);

      // Listening first, then the browser. The other order loses the race whenever the user is
      // already signed in to TONE3000: the redirect comes straight back and finds nothing on the
      // port, so the sign-in succeeds everywhere except here.
      std::string listenerError;
      if (!StartOAuthListener(listenerError))
      {
        SetStatus("Sign-in failed: " + listenerError);
        return;
      }

      SetStatus("Waiting for you to sign in with your browser...");
      if (!OpenInBrowser(authorizeUrl))
      {
        StopOAuthListener();
        SetStatus("Could not open a browser.");
        return;
      }

      // Three minutes: long enough to find a password, short enough that an abandoned attempt
      // does not hold the port for the rest of the session.
      const OAuthCallback callback = WaitForOAuthRedirect(180);
      if (!callback.error.empty())
      {
        SetStatus("Sign-in failed: " + callback.error);
        return;
      }
      if (callback.state != state)
      {
        // Someone else's redirect, or a stale tab from an earlier attempt. Either way the code is
        // not ours to redeem.
        SetStatus("Sign-in failed: the response did not match this request.");
        return;
      }

      const std::string body = "grant_type=authorization_code&code=" + UrlEncode(callback.code)
                               + "&code_verifier=" + UrlEncode(pkce.verifier)
                               + "&redirect_uri=" + UrlEncode(redirectUri)
                               + "&client_id=" + UrlEncode(clientId);

      const HttpResponse response = HttpPostForm(std::string(kApiBase) + "/oauth/token", body);
      if (!response.Ok())
      {
        SetStatus(DescribeFailure(response, "Could not complete sign-in"));
        return;
      }

      try
      {
        const nlohmann::json parsed = nlohmann::json::parse(response.BodyAsString());
        const std::string accessToken = JsonString(parsed, "access_token");
        if (accessToken.empty())
        {
          SetStatus("Sign-in returned no access token.");
          return;
        }

        const int expiresIn = parsed.contains("expires_in") && parsed["expires_in"].is_number()
                                ? parsed["expires_in"].get<int>()
                                : 3600;

        std::lock_guard<std::mutex> lock(mStateMutex);
        mAccessToken = accessToken;
        const std::string refreshToken = JsonString(parsed, "refresh_token");
        if (!refreshToken.empty())
          mRefreshToken = refreshToken;
        mTokenExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn);
        mStatusMessage = "Connected to TONE3000.";
      }
      catch (const std::exception& e)
      {
        SetStatus(std::string("Could not read the sign-in response: ") + e.what());
      }
    });
}

bool Tone3000Client::RefreshAccessToken()
{
  // One refresh at a time. Whoever loses the race finds a fresh token waiting when it gets in.
  std::lock_guard<std::mutex> refreshLock(mRefreshMutex);

  std::string clientId;
  std::string refreshToken;
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    // Someone else refreshed while this thread waited for the lock.
    if (!mAccessToken.empty() && std::chrono::steady_clock::now() < mTokenExpiry)
      return true;
    clientId = mClientId;
    refreshToken = mRefreshToken;
  }

  if (clientId.empty() || refreshToken.empty())
    return false;

  const std::string body = "grant_type=refresh_token&refresh_token=" + UrlEncode(refreshToken)
                           + "&client_id=" + UrlEncode(clientId);

  const HttpResponse response = HttpPostForm(std::string(kApiBase) + "/oauth/token", body);
  if (!response.Ok())
  {
    // A refresh token the server rejects will never work again, so drop it rather than retrying
    // it on every request from here on.
    if (response.statusCode == 400 || response.statusCode == 401)
    {
      std::lock_guard<std::mutex> lock(mStateMutex);
      mRefreshToken.clear();
      mAccessToken.clear();
      mStatusMessage = "TONE3000 sign-in has expired - connect again.";
    }
    else
    {
      SetStatus(DescribeFailure(response, "Could not refresh the TONE3000 session"));
    }
    return false;
  }

  try
  {
    const nlohmann::json parsed = nlohmann::json::parse(response.BodyAsString());
    const std::string accessToken = JsonString(parsed, "access_token");
    if (accessToken.empty())
      return false;

    const int expiresIn =
      parsed.contains("expires_in") && parsed["expires_in"].is_number() ? parsed["expires_in"].get<int>() : 3600;

    std::lock_guard<std::mutex> lock(mStateMutex);
    mAccessToken = accessToken;
    // Providers that rotate refresh tokens hand back a new one; keep the old one if they do not.
    const std::string rotated = JsonString(parsed, "refresh_token");
    if (!rotated.empty())
      mRefreshToken = rotated;
    mTokenExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn);
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

std::string Tone3000Client::AccessToken()
{
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    // Refreshed a little early, so a token does not expire in flight on a slow download.
    if (!mAccessToken.empty() && std::chrono::steady_clock::now() + std::chrono::seconds(30) < mTokenExpiry)
      return mAccessToken;
  }

  if (!RefreshAccessToken())
    return {};

  std::lock_guard<std::mutex> lock(mStateMutex);
  return mAccessToken;
}

void Tone3000Client::SetStatus(std::string message)
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  mStatusMessage = std::move(message);
}

SearchState Tone3000Client::GetSearchState() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mSearchState;
}

std::vector<TaxonomyItem> Tone3000Client::GetTags() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mTags;
}

std::vector<TaxonomyItem> Tone3000Client::GetMakes() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mMakes;
}

std::vector<TaxonomyItem> Tone3000Client::GetCreators() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mCreators;
}

std::string Tone3000Client::GetStatusMessage() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mStatusMessage;
}

bool Tone3000Client::ConsumeDownloadCompleted()
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  const bool completed = mDownloadCompleted;
  mDownloadCompleted = false;
  return completed;
}

void Tone3000Client::SearchAsync(const SearchFilters& filters, int page)
{
  if (!IsConnected())
  {
    SetStatus("Connect your TONE3000 account first.");
    return;
  }

  const int requestedPage = std::max(1, page);
  mSearchSlot.Start(
    [this, filters, requestedPage]()
    {
      SetStatus("Searching...");

      const char* format = (filters.format == SearchFormat::Ir) ? "ir" : "nam";
      std::string url = std::string(kApiBase) + "/tones/search?format=" + format
                        + "&page=" + std::to_string(requestedPage) + "&page_size=" + std::to_string(kPageSize)
                        + "&sort=" + SortValue(filters.sort);

      if (!filters.query.empty())
        url += "&query=" + UrlEncode(filters.query);
      if (const char* architecture = ArchitectureValue(filters.architecture))
        url += "&architecture=" + std::string(architecture);
      if (filters.calibratedOnly)
        url += "&calibrated=true";
      if (!filters.gears.empty())
        url += "&gears=" + UrlEncode(Join(filters.gears, '_'));
      if (!filters.sizes.empty())
        url += "&sizes=" + UrlEncode(Join(filters.sizes, '_'));
      if (!filters.tags.empty())
        url += "&tags=" + UrlEncode(Join(filters.tags, '_'));
      if (!filters.makes.empty())
        url += "&makes=" + UrlEncode(Join(filters.makes, '_'));
      // Creators are comma-separated: usernames may contain '_' or '-' themselves.
      if (!filters.creators.empty())
        url += "&creators=" + UrlEncode(Join(filters.creators, ','));

      const HttpResponse response = HttpGet(url, AccessToken());

      SearchState state;
      state.page = requestedPage;

      if (!response.Ok())
      {
        state.error = DescribeFailure(response, "Search failed");
      }
      else
      {
        try
        {
          const nlohmann::json parsed = nlohmann::json::parse(response.BodyAsString());
          state.total = JsonInt(parsed, "total");
          state.totalPages = JsonInt(parsed, "total_pages");
          state.page = JsonInt(parsed, "page", requestedPage);

          if (parsed.contains("data") && parsed["data"].is_array())
          {
            for (const auto& item : parsed["data"])
            {
              ToneSummary tone;
              tone.id = JsonLongLong(item, "id");
              tone.title = JsonString(item, "title", "(untitled)");
              tone.gear = JsonString(item, "gear");
              tone.modelsCount = JsonInt(item, "models_count");
              tone.downloadsCount = JsonInt(item, "downloads_count");
              tone.a1ModelsCount = JsonInt(item, "a1_models_count");
              tone.a2ModelsCount = JsonInt(item, "a2_models_count");
              tone.license = JsonString(item, "license");
              tone.format = JsonString(item, "format");

              // tags and makes come back as arrays of {id, name}.
              for (const char* key : {"tags", "makes"})
              {
                if (!item.contains(key) || !item[key].is_array())
                  continue;
                for (const auto& taxonomyEntry : item[key])
                {
                  std::string name = JsonString(taxonomyEntry, "name");
                  if (name.empty())
                    continue;
                  if (std::string(key) == "tags")
                    tone.tags.push_back(std::move(name));
                  else
                    tone.makes.push_back(std::move(name));
                }
              }
              if (item.contains("user") && item["user"].is_object())
              {
                const auto& user = item["user"];
                tone.userName = JsonString(user, "display_name");
                if (tone.userName.empty())
                  tone.userName = JsonString(user, "username");
              }
              state.results.push_back(std::move(tone));
            }
          }
        }
        catch (const std::exception& e)
        {
          state.error = std::string("Could not parse the search response: ") + e.what();
        }
      }

      {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mSearchState = std::move(state);
        mStatusMessage =
          mSearchState.error.empty() ? (mSearchState.results.empty() ? "No matches." : "") : mSearchState.error;
      }
    });
}

void Tone3000Client::FetchTaxonomyAsync(AsyncSlot& slot, std::string endpoint, std::string query,
                                         std::vector<TaxonomyItem>* destination)
{
  if (!IsConnected())
    return;

  slot.Start(
    [this, endpoint = std::move(endpoint), query = std::move(query), destination]()
    {
      // sort=tones puts the most-used entries first, which is what you want in a filter list.
      std::string url = std::string(kApiBase) + "/" + endpoint + "?page=1&page_size=" + std::to_string(kPageSize);
      if (endpoint != "users")
        url += "&sort=tones";
      if (!query.empty())
        url += "&query=" + UrlEncode(query);

      const HttpResponse response = HttpGet(url, AccessToken());
      if (!response.Ok())
        return; // a filter list failing to load shouldn't stomp on the main status line

      std::vector<TaxonomyItem> items;
      try
      {
        const nlohmann::json parsed = nlohmann::json::parse(response.BodyAsString());
        if (parsed.contains("data") && parsed["data"].is_array())
        {
          for (const auto& entry : parsed["data"])
          {
            TaxonomyItem item;
            // Tags and makes are keyed by "name"; users by "username".
            item.name = JsonString(entry, "name");
            if (item.name.empty())
              item.name = JsonString(entry, "username");
            if (item.name.empty())
              continue;

            item.label = JsonString(entry, "display_name");
            if (item.label.empty())
              item.label = item.name;

            item.tonesCount = JsonInt(entry, "tones_count");
            item.isVerified = JsonBool(entry, "is_verified");
            items.push_back(std::move(item));
          }
        }
      }
      catch (const std::exception&)
      {
        return;
      }

      std::lock_guard<std::mutex> lock(mStateMutex);
      *destination = std::move(items);
    });
}

void Tone3000Client::FetchTagsAsync(const std::string& query)
{
  FetchTaxonomyAsync(mTagsSlot, "tags", query, &mTags);
}

void Tone3000Client::FetchMakesAsync(const std::string& query)
{
  FetchTaxonomyAsync(mMakesSlot, "makes", query, &mMakes);
}

void Tone3000Client::FetchCreatorsAsync(const std::string& query)
{
  FetchTaxonomyAsync(mCreatorsSlot, "users", query, &mCreators);
}

void Tone3000Client::DownloadToneAsync(const ToneSummary& tone, const std::filesystem::path& destRoot)
{
  if (!IsConnected())
  {
    SetStatus("Connect your TONE3000 account first.");
    return;
  }
  if (destRoot.empty())
  {
    SetStatus("Choose a capture folder first - that's where downloads are saved.");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mDownloadMutex);

    // Already on its way, or waiting to be. Clicking twice should not fetch it twice.
    if (mDownloads.count(tone.id) > 0)
      return;
    for (const auto& queued : mDownloadQueue)
      if (queued.first.id == tone.id)
        return;

    mDownloadQueue.emplace_back(tone, destRoot);
  }
  PumpDownloads();
}

bool Tone3000Client::IsDownloading() const
{
  std::lock_guard<std::mutex> lock(mDownloadMutex);
  return !mDownloads.empty() || !mDownloadQueue.empty();
}

bool Tone3000Client::IsDownloadingTone(long long toneId) const
{
  std::lock_guard<std::mutex> lock(mDownloadMutex);
  if (mDownloads.count(toneId) > 0)
    return true;
  for (const auto& queued : mDownloadQueue)
    if (queued.first.id == toneId)
      return true;
  return false;
}

void Tone3000Client::PumpDownloads()
{
  // Two things, both cheap, both on the caller's thread: reap what has finished, then start what
  // fits. Joining here rather than detaching is what guarantees no worker outlives the client.
  std::vector<std::thread> finished;

  {
    std::lock_guard<std::mutex> lock(mDownloadMutex);

    for (auto it = mDownloads.begin(); it != mDownloads.end();)
    {
      if (it->second->finished.load(std::memory_order_acquire))
      {
        finished.push_back(std::move(it->second->thread));
        it = mDownloads.erase(it);
      }
      else
      {
        ++it;
      }
    }

    while (!mDownloadQueue.empty() && mDownloads.size() < kMaxConcurrentDownloads)
    {
      const ToneSummary tone = mDownloadQueue.front().first;
      const std::filesystem::path destRoot = mDownloadQueue.front().second;
      mDownloadQueue.pop_front();

      auto job = std::make_unique<DownloadJob>();
      DownloadJob* raw = job.get();
      job->thread = std::thread(
        [this, tone, destRoot, raw]()
        {
          RunDownload(tone, destRoot);
          raw->finished.store(true, std::memory_order_release);
        });
      mDownloads.emplace(tone.id, std::move(job));
    }
  }

  // Joined outside the lock: a worker that is still finishing may want it.
  for (auto& thread : finished)
    if (thread.joinable())
      thread.join();
}

Tone3000Client::~Tone3000Client()
{
  // Nothing queued gets started, but whatever is running has to be waited for - its thread holds
  // a pointer to this object.
  {
    std::lock_guard<std::mutex> lock(mDownloadMutex);
    mDownloadQueue.clear();
  }

  for (;;)
  {
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(mDownloadMutex);
      if (mDownloads.empty())
        break;
      auto it = mDownloads.begin();
      thread = std::move(it->second->thread);
      mDownloads.erase(it);
    }
    if (thread.joinable())
      thread.join();
  }
}

void Tone3000Client::RunDownload(const ToneSummary& tone, const std::filesystem::path& destRoot)
{
  SetStatus("Fetching model list...");

  const std::string modelsUrl = std::string(kApiBase) + "/models?tone_id=" + std::to_string(tone.id);
  const HttpResponse modelsResponse = HttpGet(modelsUrl, AccessToken());
  if (!modelsResponse.Ok())
  {
    SetStatus(DescribeFailure(modelsResponse, "Could not list models"));
    return;
  }

  std::vector<ModelInfo> models;
  try
  {
    const nlohmann::json parsed = nlohmann::json::parse(modelsResponse.BodyAsString());
    if (parsed.contains("data") && parsed["data"].is_array())
    {
      for (const auto& item : parsed["data"])
      {
        ModelInfo model;
        model.id = JsonLongLong(item, "id");
        model.name = JsonString(item, "name");
        model.modelUrl = JsonString(item, "model_url");
        model.architectureVersion = JsonString(item, "architecture_version");
        if (!model.modelUrl.empty())
          models.push_back(std::move(model));
      }
    }
  }
  catch (const std::exception& e)
  {
    SetStatus(std::string("Could not parse the model list: ") + e.what());
    return;
  }

  if (models.empty())
  {
    SetStatus("That tone has no downloadable models.");
    return;
  }

  // Each tone gets its own folder, so a pack of 40 gain stages stays one tidy unit instead of
  // 40 loose files, and one sidecar can describe the whole set.
  std::error_code ec;
  const std::filesystem::path toneFolder = destRoot / SanitizeFileName(tone.title);
  std::filesystem::create_directories(toneFolder, ec);

  CaptureMetadata meta;
  meta.title = tone.title;
  meta.gear = tone.gear;
  meta.creator = tone.userName;
  meta.license = tone.license;
  meta.tags = tone.tags;
  meta.makes = tone.makes;
  meta.architecture = tone.a2ModelsCount > 0 ? "2" : (tone.a1ModelsCount > 0 ? "1" : "");
  meta.known = true;
  meta.Save(toneFolder);

  int saved = 0;
  for (size_t i = 0; i < models.size(); i++)
  {
    SetStatus("Downloading " + std::to_string(i + 1) + "/" + std::to_string(models.size()) + "...");

    const HttpResponse fileResponse = HttpGet(models[i].modelUrl, AccessToken());
    if (!fileResponse.Ok())
    {
      SetStatus(DescribeFailure(fileResponse, "Download failed"));
      return;
    }

    // Inside the tone's own folder the model name alone is enough.
    std::string stem =
      models[i].name.empty() ? (SanitizeFileName(tone.title) + " " + std::to_string(i + 1))
                             : SanitizeFileName(models[i].name);

    // Impulse responses are WAVs; captures are .nam. The library scans for both.
    const std::string extension = (tone.format == "ir") ? ".wav" : ".nam";

    // Don't clobber an existing file; add a counter instead.
    std::filesystem::path target = toneFolder / (stem + extension);
    for (int attempt = 2; std::filesystem::exists(target, ec); attempt++)
      target = toneFolder / (stem + " (" + std::to_string(attempt) + ")" + extension);

    std::ofstream out(target, std::ios::binary);
    if (!out.is_open())
    {
      SetStatus("Could not write " + target.string());
      return;
    }
    out.write(fileResponse.body.data(), static_cast<std::streamsize>(fileResponse.body.size()));
    if (!out.good())
    {
      SetStatus("Failed while writing " + target.string());
      return;
    }
    saved++;
  }

  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mStatusMessage = "Downloaded " + std::to_string(saved) + (saved == 1 ? " capture." : " captures.");
    mDownloadCompleted = true;
  }
}

} // namespace nam_ui
