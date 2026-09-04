#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nam_ui
{

/// One tone (a capture "release") as returned by GET /tones/search.
/// The taxonomy fields are carried through to the download so the same tags and categories can be
/// filtered on locally, long after the capture has left TONE3000.
struct ToneSummary
{
  long long id = 0;
  std::string title;
  std::string gear;
  std::string userName;
  std::string license;
  /// "nam", "ir", ... - decides the extension downloaded files are saved with.
  std::string format;
  std::vector<std::string> tags;
  std::vector<std::string> makes;
  int modelsCount = 0;
  int downloadsCount = 0;
  int a1ModelsCount = 0;
  int a2ModelsCount = 0;
};

/// The folder a tone's files are downloaded into, under the capture folder. Exposed so the search
/// results can say which tones are already in the library instead of offering to fetch them twice.
std::string ToneFolderName(const std::string& title);

/// One downloadable model file belonging to a tone (GET /models?tone_id=...).
struct ModelInfo
{
  long long id = 0;
  std::string name;
  std::string modelUrl;
  std::string architectureVersion;
};

/// An entry in one of the filter lists (GET /tags, GET /makes, GET /users).
struct TaxonomyItem
{
  std::string name; ///< the exact value the search endpoint expects
  std::string label; ///< what to show a human (display name for creators, else same as name)
  int tonesCount = 0;
  bool isVerified = false; ///< creators only
};

enum class SortOrder
{
  BestMatch,
  Newest,
  Oldest,
  Trending,
  DownloadsAllTime
};

enum class Architecture
{
  Any,
  A2,
  A1,
  Custom
};

/// The app plays NAM captures in its PRE/AMP stages and impulse responses in its IR slot, so the
/// search is restricted to one of the two rather than the API's full format list.
enum class SearchFormat
{
  Nam,
  Ir
};

/// Mirrors the query parameters of GET /tones/search that this app exposes.
/// Note the API joins most multi-value filters with '_', but creators with ',' - usernames may
/// themselves contain underscores.
struct SearchFilters
{
  std::string query;
  SearchFormat format = SearchFormat::Nam;
  SortOrder sort = SortOrder::BestMatch;
  Architecture architecture = Architecture::Any;
  bool calibratedOnly = false;
  std::vector<std::string> gears; ///< amp, amp-cab, pedal, outboard, cab, space, experimental
  std::vector<std::string> sizes; ///< standard, lite, feather, nano, custom
  std::vector<std::string> tags;
  std::vector<std::string> makes;
  std::vector<std::string> creators;
};

struct SearchState
{
  std::vector<ToneSummary> results;
  int page = 1;
  int totalPages = 0;
  int total = 0;
  std::string error;
};

/// Runs one background operation at a time, ignoring requests made while it is busy.
/// Several of these let searching, browsing filter lists and downloading proceed independently.
class AsyncSlot
{
public:
  AsyncSlot() = default;
  ~AsyncSlot();

  AsyncSlot(const AsyncSlot&) = delete;
  AsyncSlot& operator=(const AsyncSlot&) = delete;

  bool Busy() const { return mBusy.load(std::memory_order_relaxed); }
  void Start(std::function<void()> work);

private:
  std::thread mWorker;
  std::atomic<bool> mBusy{false};
};

/// \brief TONE3000 API client (https://www.tone3000.com/api/v1).
///
/// Authenticates with a personal secret key sent as a bearer token. Every call runs on a
/// background thread so neither the UI nor the audio callback ever waits on the network.
class Tone3000Client
{
public:
  /// How many downloads run at once. Enough that a handful of packs arrive together, few enough
  /// to stay a polite client.
  static constexpr size_t kMaxConcurrentDownloads = 4;

  Tone3000Client() = default;
  ~Tone3000Client();

  Tone3000Client(const Tone3000Client&) = delete;
  Tone3000Client& operator=(const Tone3000Client&) = delete;

  // --- authentication --------------------------------------------------------------------
  //
  // OAuth 2.0 with PKCE. The browser does the signing in, the app never sees the password, and
  // what it keeps is a refresh token it can be told to forget - none of which is true of pasting
  // a permanent secret key into a text box.

  /// The publishable key identifying this app to TONE3000. Not a secret: it is safe in a config
  /// file and safe to share, which is exactly what distinguishes it from the old secret key.
  void SetClientId(std::string clientId);
  bool HasClientId() const;
  /// The key actually in use, for the settings panel to show. Not a secret.
  std::string GetClientId() const;

  /// Restores a session from a previously saved refresh token. Does not call out - the first
  /// request that needs a token will exchange it.
  void SetRefreshToken(std::string refreshToken);
  /// The current refresh token, to be stored encrypted. Empty when not connected.
  std::string GetRefreshToken() const;

  /// Opens the browser and waits for the redirect, on a background thread.
  void BeginAuthorizationAsync();
  bool IsAuthorizing() const { return mAuthSlot.Busy(); }

  /// True once there is a token, or a refresh token that can be exchanged for one.
  bool IsConnected() const;
  /// Drops every token. The refresh token is not revoked server-side - that is done from the
  /// TONE3000 account page - but nothing is left on this machine.
  void Disconnect();

  bool IsSearching() const { return mSearchSlot.Busy(); }

  /// True while anything at all is downloading.
  bool IsDownloading() const;
  /// True while this particular tone is downloading or waiting for a slot, so its own row in the
  /// results can say so.
  bool IsDownloadingTone(long long toneId) const;

  /// Search the library. format is pinned to "nam" - this app can't play anything else.
  void SearchAsync(const SearchFilters& filters, int page);

  /// Download every model belonging to a tone into its own subfolder of destRoot, alongside a
  /// sidecar file holding the tone's metadata.
  ///
  /// Several downloads run at once, up to kMaxConcurrentDownloads; the rest wait their turn.
  /// Asking for a tone that is already downloading or queued does nothing.
  void DownloadToneAsync(const ToneSummary& tone, const std::filesystem::path& destRoot);

  /// Reaps finished download threads and starts queued ones. Call once per frame.
  void PumpDownloads();

  /// Populate the filter lists. An empty query returns the most-used entries.
  void FetchTagsAsync(const std::string& query);
  void FetchMakesAsync(const std::string& query);
  void FetchCreatorsAsync(const std::string& query);

  SearchState GetSearchState() const;
  std::vector<TaxonomyItem> GetTags() const;
  std::vector<TaxonomyItem> GetMakes() const;
  std::vector<TaxonomyItem> GetCreators() const;

  std::string GetStatusMessage() const;

  /// Returns true exactly once after a download finishes, so the caller can rescan its library.
  bool ConsumeDownloadCompleted();

private:
  /// Shared implementation for the three taxonomy endpoints, which have identical shapes.
  /// \param destination points at one of the member vectors below; written under mStateMutex.
  void FetchTaxonomyAsync(AsyncSlot& slot, std::string endpoint, std::string query,
                          std::vector<TaxonomyItem>* destination);

  /// The body of one download, run on its own thread.
  void RunDownload(const ToneSummary& tone, const std::filesystem::path& destRoot);

  /// A token good for the next few seconds, refreshing it first if it is close to expiring.
  /// Empty when not connected or when the refresh failed. Blocking; worker threads only.
  std::string AccessToken();
  /// Exchanges the refresh token for a new access token. Caller must hold no lock.
  bool RefreshAccessToken();

  void SetStatus(std::string message);

  /// One download in flight. The thread is joined by PumpDownloads once `finished` is set, so no
  /// thread outlives the client and nothing is ever detached.
  struct DownloadJob
  {
    std::thread thread;
    std::atomic<bool> finished{false};
  };

  mutable std::mutex mStateMutex;
  std::string mClientId;
  std::string mAccessToken;
  std::string mRefreshToken;
  /// steady_clock time the access token stops being valid.
  std::chrono::steady_clock::time_point mTokenExpiry;
  /// Serialises refreshes, so several workers hitting an expired token do not each spend a
  /// refresh - the second one waits and then finds the token already good.
  std::mutex mRefreshMutex;

  SearchState mSearchState;
  std::string mStatusMessage;
  bool mDownloadCompleted = false;

  std::vector<TaxonomyItem> mTags;
  std::vector<TaxonomyItem> mMakes;
  std::vector<TaxonomyItem> mCreators;

  /// Guards the download bookkeeping only. Kept apart from mStateMutex so a worker posting status
  /// never contends with the UI asking which tones are in flight.
  mutable std::mutex mDownloadMutex;
  std::map<long long, std::unique_ptr<DownloadJob>> mDownloads;
  std::deque<std::pair<ToneSummary, std::filesystem::path>> mDownloadQueue;

  AsyncSlot mSearchSlot;
  AsyncSlot mAuthSlot;
  AsyncSlot mTagsSlot;
  AsyncSlot mMakesSlot;
  AsyncSlot mCreatorsSlot;
};

} // namespace nam_ui
