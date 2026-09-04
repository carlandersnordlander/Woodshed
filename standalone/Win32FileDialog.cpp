#include "Win32FileDialog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

namespace nam_ui
{

namespace
{
// Returns true if COM is usable on this thread (either we initialized it, or it was already
// initialized - possibly with a different concurrency model - by GLFW or another library).
bool EnsureComInitialized(bool& weInitializedIt)
{
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  weInitializedIt = (hr == S_OK);
  return hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;
}
} // namespace

std::optional<std::filesystem::path> BrowseForFolder(void* ownerHwnd, const std::wstring& title,
                                                       const std::filesystem::path& initialFolder)
{
  bool weInitializedCom = false;
  if (!EnsureComInitialized(weInitializedCom))
    return std::nullopt;

  std::optional<std::filesystem::path> result;
  IFileDialog* dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (SUCCEEDED(hr))
  {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title.c_str());

    if (!initialFolder.empty())
    {
      IShellItem* folderItem = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.wstring().c_str(), nullptr, IID_PPV_ARGS(&folderItem))))
      {
        dialog->SetFolder(folderItem);
        folderItem->Release();
      }
    }

    hr = dialog->Show(static_cast<HWND>(ownerHwnd));
    if (SUCCEEDED(hr))
    {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)))
      {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
          result = std::filesystem::path(path);
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
  }

  if (weInitializedCom)
    CoUninitialize();

  return result;
}

std::optional<std::filesystem::path> BrowseForFile(void* ownerHwnd, const std::wstring& title,
                                                     const std::wstring& filterLabel, const std::wstring& extension)
{
  bool weInitializedCom = false;
  if (!EnsureComInitialized(weInitializedCom))
    return std::nullopt;

  std::optional<std::filesystem::path> result;
  IFileDialog* dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (SUCCEEDED(hr))
  {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(title.c_str());

    const std::wstring pattern = L"*." + extension;
    const COMDLG_FILTERSPEC filters[] = {{filterLabel.c_str(), pattern.c_str()}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);

    // Only the first of a several-extension list is a sensible default to append.
    const std::wstring defaultExtension = extension.substr(0, extension.find(L';'));
    dialog->SetDefaultExtension(defaultExtension.c_str());

    hr = dialog->Show(static_cast<HWND>(ownerHwnd));
    if (SUCCEEDED(hr))
    {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)))
      {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
          result = std::filesystem::path(path);
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
  }

  if (weInitializedCom)
    CoUninitialize();

  return result;
}

} // namespace nam_ui
