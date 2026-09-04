#pragma once

#include <string>
#include <vector>

namespace nam_ui
{

struct HttpResponse
{
  long statusCode = 0;
  std::vector<char> body;
  std::string transportError; ///< non-empty if the request never completed (DNS, TLS, socket, ...)

  bool Ok() const { return transportError.empty() && statusCode >= 200 && statusCode < 300; }
  std::string BodyAsString() const { return std::string(body.begin(), body.end()); }
};

/// \brief HTTPS GET via WinHTTP.
///
/// Redirects are followed manually rather than by WinHTTP, so that the bearer token can be
/// dropped when a redirect crosses to a different host. TONE3000's model_url may well redirect to
/// a CDN/object store, and forwarding an API credential to a third-party host would leak it.
///
/// \param url Absolute http(s) URL
/// \param bearerToken Sent as "Authorization: Bearer ..."; pass an empty string for no auth
/// \param maxRedirects Redirect hops to follow before giving up
HttpResponse HttpGet(const std::string& url, const std::string& bearerToken, int maxRedirects = 5);

/// \brief HTTPS POST of an application/x-www-form-urlencoded body.
///
/// What OAuth token endpoints take. No bearer token: the request authenticates itself with what is
/// in the body, and redirects are not followed - a token endpoint that redirects is not one we
/// should be posting a code verifier to.
///
/// \param formBody already percent-encoded, e.g. "grant_type=refresh_token&refresh_token=..."
HttpResponse HttpPostForm(const std::string& url, const std::string& formBody);

/// \brief Percent-encode a string for use in a URL query value.
std::string UrlEncode(const std::string& value);

} // namespace nam_ui
