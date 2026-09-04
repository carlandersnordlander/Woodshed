#pragma once

namespace nam_ui
{

/// \brief This application's TONE3000 publishable key - its OAuth `client_id`.
///
/// It identifies the *app*, not the person using it. Every user signs in to their own TONE3000
/// account in the browser; the key just tells TONE3000 which application is asking. That is why
/// there is one of these for the whole app rather than one per user, and why a user never has to
/// know that OAuth exists.
///
/// It is not a credential and not a secret. TONE3000 documents it as safe for client-side use -
/// it authorises nothing on its own, and an authorization code is useless without the PKCE
/// verifier that only the running app holds. Shipping it in the binary is what it is for.
///
/// To fill it in: create a publishable key in your TONE3000 account settings and paste it below,
/// or define NAM_TONE3000_CLIENT_ID at build time to keep it out of the source tree:
///
///     cmake -B build -S . -DNAM_TONE3000_CLIENT_ID=t3k_pk_your_key
///
/// Left empty, the app falls back to asking each user for their own key under Settings ->
/// Advanced, which works but is not what anyone wants.
#ifdef NAM_TONE3000_CLIENT_ID
constexpr const char* kTone3000AppClientId = NAM_TONE3000_CLIENT_ID;
#else
constexpr const char* kTone3000AppClientId = "t3k_pub_bRUgZWwuX8763QgeS-D30IbD88OYLle1";
#endif

} // namespace nam_ui
