#include "WindowChrome.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#ifdef _WIN32
#include <windows.h>

#include <dwmapi.h>
#endif

namespace nam_ui
{

void ApplyWindowChrome(GLFWwindow* window)
{
#ifdef _WIN32
  HWND hwnd = glfwGetWin32Window(window);
  if (hwnd == nullptr)
    return;

  // Named here rather than taken from the headers: an older Windows SDK does not define them, and
  // an older Windows simply ignores an attribute it does not know, which is exactly the behaviour
  // wanted - the window is then what it always was.
  constexpr DWORD kUseImmersiveDarkMode = 20;
  constexpr DWORD kBorderColour = 34;
  constexpr DWORD kCaptionColour = 35;
  constexpr DWORD kTextColour = 36;

  // Dark first: it decides how the minimise, maximise and close buttons are drawn, and those are
  // the one part of the caption an application still cannot colour for itself.
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));

  // COLORREF is 0x00BBGGRR, which RGB() handles.
  const COLORREF caption = RGB(16, 16, 18);  // theme::Rail(), so the bar is the rail carried on up
  const COLORREF text = RGB(245, 241, 234);  // theme::Wood()
  DwmSetWindowAttribute(hwnd, kCaptionColour, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd, kTextColour, &text, sizeof(text));
  DwmSetWindowAttribute(hwnd, kBorderColour, &caption, sizeof(caption));
#else
  (void)window;
#endif
}

} // namespace nam_ui
