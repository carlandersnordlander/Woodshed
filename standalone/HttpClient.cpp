#include "HttpClient.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>

namespace nam_ui
{

namespace
{

std::wstring Utf8ToWide(const std::string& s)
{
  if (s.empty())
    return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

std::string WideToUtf8(const std::wstring& s)
{
  if (s.empty())
    return {};
  const int needed =
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

struct ParsedUrl
{
  std::wstring host;
  std::wstring pathWithQuery;
  INTERNET_PORT port = 0;
  bool secure = false;
};

bool ParseUrl(const std::string& url, ParsedUrl& out)
{
  const std::wstring wide = Utf8ToWide(url);

  URL_COMPONENTS components;
  ZeroMemory(&components, sizeof(components));
  components.dwStructSize = sizeof(components);

  wchar_t hostBuffer[256] = {};
  wchar_t pathBuffer[4096] = {};
  wchar_t extraBuffer[4096] = {};
  components.lpszHostName = hostBuffer;
  components.dwHostNameLength = ARRAYSIZE(hostBuffer);
  components.lpszUrlPath = pathBuffer;
  components.dwUrlPathLength = ARRAYSIZE(pathBuffer);
  components.lpszExtraInfo = extraBuffer;
  components.dwExtraInfoLength = ARRAYSIZE(extraBuffer);

  if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components))
    return false;

  out.host.assign(components.lpszHostName, components.dwHostNameLength);
  out.pathWithQuery.assign(components.lpszUrlPath, components.dwUrlPathLength);
  out.pathWithQuery.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  if (out.pathWithQuery.empty())
    out.pathWithQuery = L"/";
  out.port = components.nPort;
  out.secure = (components.nScheme == INTERNET_SCHEME_HTTPS);
  return true;
}

std::string LastErrorMessage(const char* what)
{
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s failed (Windows error %lu)", what, GetLastError());
  return buffer;
}

// Minimal RAII for WinHTTP handles - several early-return paths would otherwise leak them.
class WinHttpHandle
{
public:
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle)
  : mHandle(handle)
  {
  }
  ~WinHttpHandle()
  {
    if (mHandle)
      WinHttpCloseHandle(mHandle);
  }

  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;

  WinHttpHandle& operator=(HINTERNET handle)
  {
    if (mHandle)
      WinHttpCloseHandle(mHandle);
    mHandle = handle;
    return *this;
  }

  operator HINTERNET() const { return mHandle; }
  explicit operator bool() const { return mHandle != nullptr; }

private:
  HINTERNET mHandle = nullptr;
};

} // namespace

std::string UrlEncode(const std::string& value)
{
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value)
  {
    const bool unreserved =
      (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved)
    {
      out.push_back(static_cast<char>(c));
    }
    else
    {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

HttpResponse HttpGet(const std::string& url, const std::string& bearerToken, int maxRedirects)
{
  HttpResponse response;

  ParsedUrl parsed;
  if (!ParseUrl(url, parsed))
  {
    response.transportError = "Could not parse URL: " + url;
    return response;
  }
  const std::wstring originalHost = parsed.host;

  WinHttpHandle session(WinHttpOpen(L"Woodshed/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session)
  {
    response.transportError = LastErrorMessage("WinHttpOpen");
    return response;
  }

  for (int hop = 0; hop <= maxRedirects; hop++)
  {
    WinHttpHandle connection(WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0));
    if (!connection)
    {
      response.transportError = LastErrorMessage("WinHttpConnect");
      return response;
    }

    WinHttpHandle request(WinHttpOpenRequest(connection, L"GET", parsed.pathWithQuery.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             parsed.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request)
    {
      response.transportError = LastErrorMessage("WinHttpOpenRequest");
      return response;
    }

    // We follow redirects ourselves so we can decide per hop whether the token travels with them.
    DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirects, sizeof(disableRedirects));

    // Only ever send the credential to the host we originally aimed it at.
    if (!bearerToken.empty() && parsed.host == originalHost)
    {
      const std::wstring header = L"Authorization: Bearer " + Utf8ToWide(bearerToken);
      WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(header.size()),
                               WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
      response.transportError = LastErrorMessage("WinHttpSendRequest");
      return response;
    }
    if (!WinHttpReceiveResponse(request, nullptr))
    {
      response.transportError = LastErrorMessage("WinHttpReceiveResponse");
      return response;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
    {
      response.transportError = LastErrorMessage("WinHttpQueryHeaders(status)");
      return response;
    }
    response.statusCode = static_cast<long>(statusCode);

    const bool isRedirect =
      (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308);
    if (isRedirect && hop < maxRedirects)
    {
      DWORD locationSize = 0;
      WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                          &locationSize, WINHTTP_NO_HEADER_INDEX);
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locationSize > 0)
      {
        std::wstring location(locationSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(),
                                &locationSize, WINHTTP_NO_HEADER_INDEX))
        {
          while (!location.empty() && location.back() == L'\0')
            location.pop_back();

          ParsedUrl next;
          if (ParseUrl(WideToUtf8(location), next))
          {
            parsed = next;
            continue; // follow it
          }
        }
      }
      response.transportError = "Redirect response without a usable Location header.";
      return response;
    }

    // Terminal response - read the body.
    for (;;)
    {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available))
      {
        response.transportError = LastErrorMessage("WinHttpQueryDataAvailable");
        return response;
      }
      if (available == 0)
        break;

      const size_t offset = response.body.size();
      response.body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request, response.body.data() + offset, available, &read))
      {
        response.transportError = LastErrorMessage("WinHttpReadData");
        return response;
      }
      response.body.resize(offset + read);
      if (read == 0)
        break;
    }

    return response;
  }

  response.transportError = "Too many redirects.";
  return response;
}

HttpResponse HttpPostForm(const std::string& url, const std::string& formBody)
{
  HttpResponse response;

  ParsedUrl parsed;
  if (!ParseUrl(url, parsed))
  {
    response.transportError = "Could not parse URL: " + url;
    return response;
  }

  WinHttpHandle session(WinHttpOpen(L"Woodshed/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session)
  {
    response.transportError = LastErrorMessage("WinHttpOpen");
    return response;
  }

  WinHttpHandle connection(WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0));
  if (!connection)
  {
    response.transportError = LastErrorMessage("WinHttpConnect");
    return response;
  }

  WinHttpHandle request(WinHttpOpenRequest(connection, L"POST", parsed.pathWithQuery.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           parsed.secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request)
  {
    response.transportError = LastErrorMessage("WinHttpOpenRequest");
    return response;
  }

  DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
  WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirects, sizeof(disableRedirects));

  static const wchar_t* kContentType = L"Content-Type: application/x-www-form-urlencoded\r\n";
  if (!WinHttpSendRequest(request, kContentType, static_cast<DWORD>(-1L),
                          const_cast<char*>(formBody.data()), static_cast<DWORD>(formBody.size()),
                          static_cast<DWORD>(formBody.size()), 0))
  {
    response.transportError = LastErrorMessage("WinHttpSendRequest");
    return response;
  }
  if (!WinHttpReceiveResponse(request, nullptr))
  {
    response.transportError = LastErrorMessage("WinHttpReceiveResponse");
    return response;
  }

  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
  {
    response.transportError = LastErrorMessage("WinHttpQueryHeaders(status)");
    return response;
  }
  response.statusCode = static_cast<long>(statusCode);

  for (;;)
  {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available))
    {
      response.transportError = LastErrorMessage("WinHttpQueryDataAvailable");
      return response;
    }
    if (available == 0)
      break;

    const size_t offset = response.body.size();
    response.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request, response.body.data() + offset, available, &read))
    {
      response.transportError = LastErrorMessage("WinHttpReadData");
      return response;
    }
    response.body.resize(offset + read);
    if (read == 0)
      break;
  }

  return response;
}

} // namespace nam_ui
