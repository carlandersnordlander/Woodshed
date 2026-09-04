#include "Subprocess.h"

#include <windows.h>

namespace nam_ui
{

namespace
{

/// Handle wrapper, because every failure path below would otherwise have to remember to close.
class Handle
{
public:
  Handle() = default;
  explicit Handle(HANDLE handle)
  : mHandle(handle)
  {
  }
  ~Handle() { Close(); }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  void Close()
  {
    if (mHandle != nullptr && mHandle != INVALID_HANDLE_VALUE)
      CloseHandle(mHandle);
    mHandle = nullptr;
  }

  HANDLE* Address() { return &mHandle; }
  operator HANDLE() const { return mHandle; }
  explicit operator bool() const { return mHandle != nullptr && mHandle != INVALID_HANDLE_VALUE; }

private:
  HANDLE mHandle = nullptr;
};

} // namespace

std::wstring ToWide(const std::string& text)
{
  if (text.empty())
    return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

std::wstring QuoteArgument(const std::wstring& value)
{
  std::wstring out = L"\"";
  for (const wchar_t c : value)
  {
    if (c == L'"')
      out += L"\\\"";
    else
      out += c;
  }
  out += L"\"";
  return out;
}

int RunProcess(const std::vector<std::string>& candidates, const std::wstring& arguments,
               const std::function<void(const std::string&)>& onLine, std::string& error)
{
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  Handle readEnd;
  Handle writeEnd;
  if (!CreatePipe(readEnd.Address(), writeEnd.Address(), &security, 0))
  {
    error = "Could not create a pipe to read the tool's output.";
    return -1;
  }
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE; // no console window flashing up over the app
  startup.hStdOutput = writeEnd;
  startup.hStdError = writeEnd;
  startup.hStdInput = nullptr;

  PROCESS_INFORMATION process{};
  bool started = false;
  DWORD lastError = 0;

  for (const auto& candidate : candidates)
  {
    if (candidate.empty())
      continue;

    const std::wstring wide = ToWide(candidate);
    const bool hasArguments = wide.find(L' ') != std::wstring::npos;

    // CreateProcessW may write into this buffer, so each attempt gets its own copy.
    std::wstring commandLine = (hasArguments ? wide : QuoteArgument(wide)) + arguments;
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                       &startup, &process))
    {
      started = true;
      break;
    }
    lastError = GetLastError();
  }

  if (!started)
  {
    error = (lastError == ERROR_FILE_NOT_FOUND)
              ? std::string("not found")
              : "Windows error " + std::to_string(lastError);
    return -1;
  }

  Handle processHandle(process.hProcess);
  Handle threadHandle(process.hThread);
  writeEnd.Close(); // ours must go, or the read below never sees end of file

  std::string line;
  char buffer[512];
  DWORD read = 0;
  while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0)
  {
    for (DWORD i = 0; i < read; i++)
    {
      const char c = buffer[i];
      if (c == '\n' || c == '\r')
      {
        if (!line.empty() && onLine)
          onLine(line);
        line.clear();
      }
      else
      {
        line.push_back(c);
      }
    }
  }
  if (!line.empty() && onLine)
    onLine(line);

  WaitForSingleObject(processHandle, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(processHandle, &exitCode);
  return static_cast<int>(exitCode);
}

} // namespace nam_ui
