#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace nam_ui
{

// Shows the native Windows folder-picker dialog. ownerHwnd is an HWND (typed as void* here so
// this header stays free of <windows.h>); pass nullptr for no owner. Returns std::nullopt if
// the dialog was cancelled or failed.
std::optional<std::filesystem::path> BrowseForFolder(void* ownerHwnd, const std::wstring& title,
                                                       const std::filesystem::path& initialFolder = {});

/// Native open-file dialog restricted to an extension (e.g. L"wav"), or to several written the way
/// the shell wants them: L"wav;*.mp3;*.flac".
/// Returns std::nullopt if the dialog was cancelled or failed.
std::optional<std::filesystem::path> BrowseForFile(void* ownerHwnd, const std::wstring& title,
                                                     const std::wstring& filterLabel, const std::wstring& extension);

} // namespace nam_ui
