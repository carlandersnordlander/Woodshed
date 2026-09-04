// Standalone realtime capture-browser UI for NeuralAmpModelerCore.
// GLFW + OpenGL3 window/context, Dear ImGui for the UI, RtAudio for realtime audio I/O.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include "Config.h"
#include "WindowChrome.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

namespace
{
void GlfwErrorCallback(int error, const char* description)
{
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}
} // namespace

int main(int, char**)
{
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit())
    return 1;

  // GL 3.0 context + GLSL 130, matching what Dear ImGui's OpenGL3 backend defaults to when no
  // explicit GLSL version string is passed in.
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

  const float mainScale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
  GLFWwindow* window = glfwCreateWindow(static_cast<int>(1200 * mainScale), static_cast<int>(800 * mainScale),
                                        "Woodshed", nullptr, nullptr);
  if (window == nullptr)
  {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  // The frame, before the first paint: the caption is drawn by the compositor as soon as the window
  // is shown, and setting it afterwards means a flash of grey. The icon needs no call at all - GLFW
  // takes it from the GLFW_ICON resource compiled into the executable.
  nam_ui::ApplyWindowChrome(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // --- where the window layout is remembered ---
  //
  // Dear ImGui writes this beside whatever the working directory happens to be, which for an
  // installed app is Program Files: not writable, so the layout is silently never saved, and for an
  // app started from a shortcut it is wherever the shortcut points. It belongs with the rest of the
  // app's state. Held in a static because ImGui keeps the pointer rather than the string.
  static const std::string iniPath = (nam_ui::AppConfig::GetConfigFolder() / "imgui.ini").string();
  {
    std::error_code ec;
    std::filesystem::create_directories(nam_ui::AppConfig::GetConfigFolder(), ec);
  }
  io.IniFilename = iniPath.c_str();

  nam_ui::theme::LoadFonts();
  nam_ui::theme::Apply();

  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(mainScale); // after Apply(), so the theme's base sizes get DPI-scaled
  style.FontScaleDpi = mainScale;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 130");

  {
    nam_ui::MainWindow mainWindow(window);

    while (!glfwWindowShouldClose(window))
    {
      glfwPollEvents();
      if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
      {
        ImGui_ImplGlfw_Sleep(10);
        continue;
      }

      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();

      mainWindow.Draw();

      ImGui::Render();
      int displayW, displayH;
      glfwGetFramebufferSize(window, &displayW, &displayH);
      glViewport(0, 0, displayW, displayH);
      glClearColor(0.082f, 0.090f, 0.106f, 1.0f); // matches the theme's window background
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

      glfwSwapBuffers(window);
    }
  } // MainWindow destructor (saves config) runs before ImGui/GLFW are torn down.

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
