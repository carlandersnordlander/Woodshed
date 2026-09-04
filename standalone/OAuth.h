#pragma once

#include <string>

namespace nam_ui
{

/// The loopback port the app listens on for the OAuth redirect.
///
/// Fixed rather than picked at random, because TONE3000 only accepts redirect URIs that have been
/// registered in the account's settings beforehand - and you cannot register a port you do not yet
/// know. Chosen high and unusual to keep the odds of a clash low.
constexpr unsigned short kOAuthRedirectPort = 8731;

/// The exact redirect URI to register with TONE3000.
std::string OAuthRedirectUri();

/// One PKCE pair, generated per authorization attempt.
struct PkcePair
{
  std::string verifier; ///< the secret, sent only to the token endpoint
  std::string challenge; ///< base64url(SHA-256(verifier)), sent to the authorize endpoint
};

/// Generates a fresh verifier and its challenge. Empty strings if the system RNG or hash failed,
/// which the caller must treat as "do not start the flow".
PkcePair MakePkcePair();

/// A random URL-safe string, for the `state` parameter.
std::string MakeRandomToken(size_t length);

/// What came back on the redirect.
struct OAuthCallback
{
  std::string code;
  std::string state;
  std::string error; ///< the provider's error, or a local failure; empty on success
};

/// \brief Opens the loopback socket, ready for the redirect.
///
/// Has to happen *before* the browser is sent to the authorize endpoint. A user who is already
/// signed in to TONE3000 is redirected back within milliseconds, and a redirect that arrives
/// before the port is listening gets a connection refused - the sign-in then succeeds everywhere
/// except in the app, which sits waiting for a request that already came and went.
///
/// \return false on failure, with `error` set
bool StartOAuthListener(std::string& error);

/// \brief Waits for the browser to reach the socket opened by StartOAuthListener, and reads the
/// result.
///
/// Serves exactly one request and answers it with a small page telling the user to return to the
/// app, because a browser left on a blank or failed page reads as the whole thing having gone
/// wrong. Blocking, so it belongs on a background thread. Closes the listener either way.
///
/// \param timeoutSeconds gives up after this long, so an abandoned sign-in does not leave a socket
///        and a thread alive for the rest of the session
OAuthCallback WaitForOAuthRedirect(int timeoutSeconds);

/// Cancels a WaitForOAuthRedirect that is in progress, so the app can shut down without waiting
/// out the timeout. Safe to call from another thread.
void CancelOAuthRedirect();

/// Tears down a listener that was started but will never be waited on - the browser failed to
/// open, say. Not needed on the normal path: WaitForOAuthRedirect cleans up after itself.
void StopOAuthListener();

/// Hands a URL to the system browser.
bool OpenInBrowser(const std::string& url);

} // namespace nam_ui
