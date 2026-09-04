#include "SecretStore.h"

#include <windows.h>
#include <wincrypt.h>

#include <vector>

namespace nam_ui
{

namespace
{
// A label Windows stores beside the blob, not part of the key: an old token stays readable after
// the app was renamed.
constexpr const wchar_t* kDescription = L"Woodshed TONE3000 token";
} // namespace

std::string EncryptSecret(const std::string& plaintext)
{
  if (plaintext.empty())
    return {};

  DATA_BLOB input;
  input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
  input.cbData = static_cast<DWORD>(plaintext.size());

  DATA_BLOB output{};
  if (!CryptProtectData(&input, kDescription, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
    return {};

  std::string base64;
  DWORD base64Size = 0;
  if (CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                           &base64Size)
      && base64Size > 0)
  {
    base64.resize(base64Size);
    if (CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64.data(),
                             &base64Size))
    {
      base64.resize(base64Size); // drops the trailing NUL the API counts
    }
    else
    {
      base64.clear();
    }
  }

  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return base64;
}

std::string DecryptSecret(const std::string& base64Blob)
{
  if (base64Blob.empty())
    return {};

  DWORD binarySize = 0;
  if (!CryptStringToBinaryA(base64Blob.data(), static_cast<DWORD>(base64Blob.size()), CRYPT_STRING_BASE64, nullptr,
                            &binarySize, nullptr, nullptr)
      || binarySize == 0)
    return {};

  std::vector<BYTE> binary(binarySize);
  if (!CryptStringToBinaryA(base64Blob.data(), static_cast<DWORD>(base64Blob.size()), CRYPT_STRING_BASE64, binary.data(),
                            &binarySize, nullptr, nullptr))
    return {};

  DATA_BLOB input;
  input.pbData = binary.data();
  input.cbData = binarySize;

  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
    return {};

  std::string plaintext(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return plaintext;
}

} // namespace nam_ui
