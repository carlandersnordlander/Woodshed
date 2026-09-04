#include "OAuth.h"

#include <atomic>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>

namespace nam_ui
{

namespace
{

/// Set while a listener is running, so CancelOAuthRedirect can break it out of accept().
std::atomic<SOCKET> gListenSocket{INVALID_SOCKET};

/// base64url, per RFC 7636: the URL-safe alphabet and no padding.
std::string Base64Url(const unsigned char* data, size_t size)
{
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string out;
  out.reserve((size + 2) / 3 * 4);

  for (size_t i = 0; i < size; i += 3)
  {
    const unsigned int byte0 = data[i];
    const unsigned int byte1 = (i + 1 < size) ? data[i + 1] : 0;
    const unsigned int byte2 = (i + 2 < size) ? data[i + 2] : 0;
    const unsigned int triple = (byte0 << 16) | (byte1 << 8) | byte2;

    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    if (i + 1 < size)
      out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
    if (i + 2 < size)
      out.push_back(kAlphabet[triple & 0x3F]);
  }
  return out;
}

/// Cryptographically strong random bytes. False if the system said no, in which case the caller
/// must not fall back to something weaker - a guessable verifier defeats the point of PKCE.
bool RandomBytes(unsigned char* out, size_t size)
{
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool Sha256(const std::string& input, unsigned char (&digest)[32])
{
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    return false;

  const NTSTATUS status =
    BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
               static_cast<ULONG>(input.size()), digest, sizeof(digest));

  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status == 0;
}

/// The value of one query parameter out of a request target like "/callback?code=x&state=y".
/// Takes the target alone, not the whole request line - see where it is called.
std::string QueryValue(const std::string& target, const std::string& key)
{
  const std::string needle = key + "=";
  size_t at = target.find("?");
  if (at == std::string::npos)
    return {};

  for (size_t position = at + 1; position < target.size();)
  {
    const size_t end = std::min(target.find('&', position), target.size());
    if (target.compare(position, needle.size(), needle) == 0)
    {
      const std::string raw = target.substr(position + needle.size(), end - position - needle.size());

      // Percent-decoding, enough for what turns up in an authorization code or an error string.
      std::string decoded;
      decoded.reserve(raw.size());
      for (size_t i = 0; i < raw.size(); i++)
      {
        if (raw[i] == '+')
        {
          decoded.push_back(' ');
        }
        else if (raw[i] == '%' && i + 2 < raw.size())
        {
          decoded.push_back(static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16)));
          i += 2;
        }
        else
        {
          decoded.push_back(raw[i]);
        }
      }
      return decoded;
    }
    position = end + 1;
  }
  return {};
}

void SendPage(SOCKET client, const std::string& title, const std::string& message)
{
  const std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + title
                           + "</title><style>body{background:#0b0b0d;color:#eee;font-family:system-ui,sans-serif;"
                             "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
                             "div{text-align:center}h1{font-weight:600;font-size:20px}p{color:#8a8c95}</style></head>"
                             "<body><div><h1>"
                           + title + "</h1><p>" + message + "</p></div></body></html>";

  const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                               + std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;

  send(client, response.data(), static_cast<int>(response.size()), 0);
}

} // namespace

std::string OAuthRedirectUri()
{
  return "http://localhost:" + std::to_string(kOAuthRedirectPort) + "/callback";
}

std::string MakeRandomToken(size_t length)
{
  std::vector<unsigned char> bytes((length * 3 + 3) / 4 + 3);
  if (!RandomBytes(bytes.data(), bytes.size()))
    return {};

  std::string token = Base64Url(bytes.data(), bytes.size());
  if (token.size() > length)
    token.resize(length);
  return token;
}

PkcePair MakePkcePair()
{
  PkcePair pair;

  // 64 characters, comfortably inside RFC 7636's 43-128 range.
  pair.verifier = MakeRandomToken(64);
  if (pair.verifier.empty())
    return {};

  unsigned char digest[32];
  if (!Sha256(pair.verifier, digest))
    return {};

  pair.challenge = Base64Url(digest, sizeof(digest));
  return pair;
}

bool StartOAuthListener(std::string& error)
{
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
  {
    error = "Could not start Windows networking.";
    return false;
  }

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET)
  {
    WSACleanup();
    error = "Could not open a socket for the sign-in callback.";
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kOAuthRedirectPort);
  // Loopback only. Binding this to every interface would put an open port on the network for as
  // long as a sign-in is in progress, which nothing here needs.
  InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr);

  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
      || listen(listener, 1) == SOCKET_ERROR)
  {
    closesocket(listener);
    WSACleanup();
    error = "Port " + std::to_string(kOAuthRedirectPort) + " is already in use.";
    return false;
  }

  gListenSocket.store(listener);
  return true;
}

OAuthCallback WaitForOAuthRedirect(int timeoutSeconds)
{
  OAuthCallback result;

  const SOCKET listener = gListenSocket.load();
  if (listener == INVALID_SOCKET)
  {
    result.error = "The sign-in listener was not running.";
    return result;
  }

  // Paired with the WSAStartup in StartOAuthListener, whichever way this function leaves.
  struct WinsockScope
  {
    ~WinsockScope() { WSACleanup(); }
  } winsockScope;

  // select() rather than a blocking accept, so an abandoned sign-in gives up on its own.
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(listener, &readable);
  timeval timeout{timeoutSeconds, 0};

  const int ready = select(0, &readable, nullptr, nullptr, &timeout);
  if (ready <= 0)
  {
    gListenSocket.store(INVALID_SOCKET);
    closesocket(listener);
    result.error = (ready == 0) ? "Timed out waiting for the browser." : "Sign-in cancelled.";
    return result;
  }

  SOCKET client = accept(listener, nullptr, nullptr);
  gListenSocket.store(INVALID_SOCKET);
  closesocket(listener);

  if (client == INVALID_SOCKET)
  {
    result.error = "Sign-in cancelled.";
    return result;
  }

  // The request line carries everything we need, and it arrives in the first packet.
  char buffer[4096];
  const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
  if (received <= 0)
  {
    closesocket(client);
    result.error = "The browser connected but sent nothing.";
    return result;
  }
  buffer[received] = '\0';

  const std::string request(buffer, static_cast<size_t>(received));
  const size_t lineEnd = request.find("\r\n");
  const std::string requestLine = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);

  // "GET /callback?code=...&state=... HTTP/1.1" - the target is what sits between the two spaces.
  // Reading query values straight off the whole line appends " HTTP/1.1" to whichever parameter
  // happens to come last, which is the sort of thing that fails silently and looks like the
  // provider's fault.
  const size_t targetStart = requestLine.find(' ');
  std::string target;
  if (targetStart != std::string::npos)
  {
    const size_t targetEnd = requestLine.find(' ', targetStart + 1);
    target = requestLine.substr(targetStart + 1, targetEnd == std::string::npos
                                                   ? std::string::npos
                                                   : targetEnd - targetStart - 1);
  }

  result.code = QueryValue(target, "code");
  result.state = QueryValue(target, "state");
  const std::string providerError = QueryValue(target, "error");

  if (!providerError.empty())
    result.error = providerError;
  else if (result.code.empty())
    result.error = "The redirect carried no authorization code.";

  if (result.error.empty())
    SendPage(client, "Connected", "You can close this tab and go back to the app.");
  else
    SendPage(client, "Sign-in failed", result.error);

  closesocket(client);
  return result;
}

void CancelOAuthRedirect()
{
  const SOCKET listener = gListenSocket.exchange(INVALID_SOCKET);
  if (listener != INVALID_SOCKET)
    closesocket(listener); // breaks the select() out immediately
}

void StopOAuthListener()
{
  CancelOAuthRedirect();
  // Matches the WSAStartup in StartOAuthListener. On the normal path WaitForOAuthRedirect does
  // this instead; doing it twice would unbalance Winsock's reference count.
  WSACleanup();
}

bool OpenInBrowser(const std::string& url)
{
  const int wide = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
  if (wide <= 0)
    return false;

  std::wstring wideUrl(static_cast<size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wideUrl.data(), wide);
  while (!wideUrl.empty() && wideUrl.back() == L'\0')
    wideUrl.pop_back();

  const HINSTANCE result = ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace nam_ui
