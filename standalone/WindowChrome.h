#pragma once

struct GLFWwindow;

namespace nam_ui
{

/// \brief Takes the title bar out of the nineties.
///
/// Windows 11 lets an application colour its own caption, its text and the hairline around the
/// window. Without it the app is a black window in a grey frame with black text on it, which is the
/// join everybody sees first. With it the frame is the same black as the app and the only things
/// left on it are the name and the three buttons.
///
/// Done through the compositor rather than by taking the frame off and drawing our own: this keeps
/// snapping, dragging to another monitor, the resize edges and the window animations, all of which
/// a hand-drawn title bar has to reimplement and none of which it reimplements as well.
///
/// Silently does nothing on a Windows that does not know the attributes, which is the correct
/// outcome - the window is then exactly what it was before.
void ApplyWindowChrome(GLFWwindow* window);

} // namespace nam_ui
