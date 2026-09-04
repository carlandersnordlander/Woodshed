#pragma once

#include <string>

namespace nam_ui
{

/// \brief Encrypt a secret for storage on disk, returning a base64 blob.
///
/// Uses Windows DPAPI, so the blob can only be decrypted by the same Windows user on the same
/// machine. This keeps the TONE3000 API key out of config.json in plaintext; it is not protection
/// against someone already running code as this user.
/// Returns an empty string on failure.
std::string EncryptSecret(const std::string& plaintext);

/// \brief Reverse of EncryptSecret. Returns an empty string if the blob can't be decrypted
/// (wrong user, different machine, corrupt or truncated value).
std::string DecryptSecret(const std::string& base64Blob);

} // namespace nam_ui
