#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace nam_ui::theme
{

namespace
{

ImFont* gBodyFont = nullptr;
ImFont* gHeadingFont = nullptr;

/// 0-255 sRGB to ImVec4, so the palette below reads like the hex values it came from.
inline ImVec4 Rgb(int r, int g, int b, float a = 1.0f)
{
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

std::filesystem::path FontDirectory()
{
  if (const char* windir = std::getenv("WINDIR"))
    return std::filesystem::path(windir) / "Fonts";
  return "C:\\Windows\\Fonts";
}

/// Returns the first of `candidates` that exists in the Windows font directory.
std::filesystem::path FindFont(const std::vector<const char*>& candidates)
{
  const auto directory = FontDirectory();
  std::error_code ec;
  for (const char* candidate : candidates)
  {
    const auto path = directory / candidate;
    if (std::filesystem::exists(path, ec))
      return path;
  }
  return {};
}

} // namespace

ImVec4 Accent() { return Rgb(240, 160, 62); }
ImVec4 AccentDim() { return Rgb(240, 160, 62, 0.22f); }
ImVec4 Wood() { return Rgb(245, 241, 234); }
ImVec4 Success() { return Rgb(108, 198, 132); }
ImVec4 Danger() { return Rgb(232, 106, 106); }
ImVec4 Warning() { return Rgb(232, 174, 88); }

// Black, and barely anything else. The steps between these are small on purpose: a screen built
// out of grey boxes on grey boxes is a screen where nothing is more important than anything else.
// Separation comes from space and from type, and the little that is left of the ladder is only
// there so a pressed control has somewhere to go.
// One black for every background. Panels, wells and the space between them were three shades of
// near-black, which reads as three surfaces that could not decide whether they were the same one.
// What varies now is what is drawn *on* the black, never the black itself.
ImVec4 Base() { return Rgb(6, 6, 7); }
ImVec4 Surface() { return Rgb(6, 6, 7); }
ImVec4 Sunken() { return Rgb(6, 6, 7); }
/// The chrome, and a popup: both float over the work and have to be told from it.
ImVec4 Rail() { return Rgb(16, 16, 18); }
ImVec4 Raised() { return Rgb(20, 20, 23); }
ImVec4 Hover() { return Rgb(32, 32, 36); }
ImVec4 Line() { return Rgb(32, 32, 36); }

ImVec4 Text() { return Rgb(244, 244, 246); }
ImVec4 TextDim() { return Rgb(138, 138, 146); }
ImVec4 TextFaint() { return Rgb(85, 85, 92); }

ImVec4 Control() { return Rgb(255, 255, 255); }

void LoadFonts()
{
  ImGuiIO& io = ImGui::GetIO();

  // Segoe UI is present on every supported Windows; the variable-font names are the Win11 ones.
  const auto bodyPath = FindFont({"segoeui.ttf", "SegoeUI-VF.ttf", "tahoma.ttf", "arial.ttf"});
  const auto headingPath = FindFont({"segoeuisb.ttf", "segoeuib.ttf", "seguisb.ttf", "arialbd.ttf"});

  if (!bodyPath.empty())
    gBodyFont = io.Fonts->AddFontFromFileTTF(bodyPath.string().c_str(), 15.0f);
  if (!gBodyFont)
    gBodyFont = io.Fonts->AddFontDefault();

  if (!headingPath.empty())
    gHeadingFont = io.Fonts->AddFontFromFileTTF(headingPath.string().c_str(), 16.0f);
  if (!gHeadingFont)
    gHeadingFont = gBodyFont;

  io.FontDefault = gBodyFont;
}

ImFont* Heading()
{
  return gHeadingFont ? gHeadingFont : Body();
}

ImFont* Body()
{
  return gBodyFont ? gBodyFont : ImGui::GetIO().FontDefault;
}

void PushHeading(float sizeScale)
{
  // FontSizeBase is the size before global DPI factors; passing GetFontSize() here would apply
  // those factors a second time.
  ImGui::PushFont(Heading(), ImGui::GetStyle().FontSizeBase * sizeScale);
}

void PopFont()
{
  ImGui::PopFont();
}

void Apply()
{
  ImGuiStyle& style = ImGui::GetStyle();
  ImVec4* colors = style.Colors;

  // Dark panels, white controls, accent used only for values and state. Anything that is not
  // information gets out of the way.
  const ImVec4 accent = Accent();

  colors[ImGuiCol_Text] = Text();
  colors[ImGuiCol_TextDisabled] = TextFaint();
  colors[ImGuiCol_WindowBg] = Base();
  colors[ImGuiCol_ChildBg] = Surface();
  colors[ImGuiCol_PopupBg] = Raised();
  colors[ImGuiCol_Border] = Line();
  colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  // A field is the one thing that keeps a fill, because it has to look like somewhere text can go.
  // A breath of white rather than a grey block.
  colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.08f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.11f);

  colors[ImGuiCol_TitleBg] = Surface();
  colors[ImGuiCol_TitleBgActive] = Surface();
  colors[ImGuiCol_TitleBgCollapsed] = Surface();
  colors[ImGuiCol_MenuBarBg] = Surface();

  colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ScrollbarGrab] = Rgb(52, 58, 68);
  colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(68, 75, 87);
  colors[ImGuiCol_ScrollbarGrabActive] = Rgb(86, 94, 108);

  colors[ImGuiCol_CheckMark] = accent;
  colors[ImGuiCol_SliderGrab] = accent;
  colors[ImGuiCol_SliderGrabActive] = Rgb(255, 182, 96);

  // No fill until you reach for it. A button at rest is its own label in white on black; the grey
  // rectangle behind it was never carrying information, and forty of them make a control panel out
  // of a screen that mostly wants to show you a waveform.
  colors[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.09f);
  colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.16f);

  // Selectables answer to the same white-on-black rule as buttons. Nothing is drawn as "selected"
  // by a fill any more - what is selected gets an outline, which is a thing you can put round a
  // whole row rather than behind one word of it.
  colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.09f);
  colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.16f);

  colors[ImGuiCol_Separator] = Rgb(30, 34, 41);
  colors[ImGuiCol_SeparatorHovered] = Line();
  colors[ImGuiCol_SeparatorActive] = Line();

  colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ResizeGripHovered] = AccentDim();
  colors[ImGuiCol_ResizeGripActive] = accent;

  colors[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TabHovered] = Raised();
  colors[ImGuiCol_TabSelected] = Raised();
  colors[ImGuiCol_TabSelectedOverline] = accent;
  colors[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TabDimmedSelected] = Surface();
  colors[ImGuiCol_TabDimmedSelectedOverline] = AccentDim();

  // ImGui dims behind a modal with a pale grey by default, which on a black panel reads as a
  // wash of light rather than as the rest of the app stepping back. Black, and darker than the
  // default, so what is in front is the only thing lit.
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);

  colors[ImGuiCol_PlotHistogram] = accent;
  colors[ImGuiCol_PlotHistogramHovered] = Rgb(255, 180, 92);
  colors[ImGuiCol_TextSelectedBg] = AccentDim();
  colors[ImGuiCol_NavCursor] = accent;
  colors[ImGuiCol_TableHeaderBg] = Raised();
  colors[ImGuiCol_TableBorderStrong] = Line();
  colors[ImGuiCol_TableBorderLight] = Rgb(28, 31, 38);
  colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.018f);

  // Tight horizontally, because every one of these is paid for on every row of every list. Looser
  // vertically, because that is the axis a dense screen actually chokes on: rows too close to each
  // other stop reading as separate things and become one grey mass.
  style.WindowPadding = ImVec2(14, 12);
  style.FramePadding = ImVec2(9, 5);
  style.CellPadding = ImVec2(8, 4);
  style.ItemSpacing = ImVec2(8, 7);
  style.ItemInnerSpacing = ImVec2(7, 5);
  style.IndentSpacing = 16.0f;
  style.ScrollbarSize = 11.0f;
  style.GrabMinSize = 11.0f;

  // No outlines anywhere. A panel is told apart from what is behind it by being lighter, which is
  // both quieter than a border and what makes the layers read as layers.
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;

  // One rounding for surfaces and a smaller one for the controls on them. Mixed radii on nested
  // shapes is one of the things that reads as unfinished.
  style.WindowRounding = 8.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 5.0f;
  style.PopupRounding = 8.0f;
  style.ScrollbarRounding = 6.0f;
  style.GrabRounding = 5.0f;
  style.TabRounding = 6.0f;

  style.FontSizeBase = 15.0f;
  style.DisabledAlpha = 0.38f;
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
  style.SeparatorTextBorderSize = 1.0f;
  style.SeparatorTextPadding = ImVec2(12, 4);
}

void SectionLabel(const char* text)
{
  // Space above, none below: a heading belongs to what follows it, and the gap that says so is the
  // one in front. A rule under every heading turned the page into a stack of boxes, so the type
  // carries it instead - smaller, dimmer, and set apart by the space around it.
  ImGui::Dummy(ImVec2(0.0f, 8.0f));
  PushHeading(0.82f);
  ImGui::PushStyleColor(ImGuiCol_Text, TextFaint());
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  PopFont();
  ImGui::Dummy(ImVec2(0.0f, 3.0f));
}

void Label(const char* text)
{
  ImGui::PushStyleColor(ImGuiCol_Text, TextDim());
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

void LabelFor(const char* text, float width)
{
  const float from = ImGui::GetCursorPosX();
  Label(text);
  ImGui::SameLine();
  if (width > 0.0f)
    ImGui::SetCursorPosX(from + width);
}

void Value(const char* text, float sizeScale)
{
  PushHeading(sizeScale);
  ImGui::TextUnformatted(text);
  PopFont();
}

void Divider()
{
  ImGui::Dummy(ImVec2(0.0f, 3.0f));
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0.0f, 3.0f));
}

void Hint(const char* text)
{
  ImGui::PushStyleColor(ImGuiCol_Text, TextFaint());
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

bool BeginCard(const char* id, const char* heading, ImVec2 size)
{
  // More padding than a plain child gets. A surface whose contents start at its own edge reads as
  // a box that something was dropped into; the inset is most of what makes it read as a card.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
  const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
  if (open && heading != nullptr)
  {
    // No space above: the card's own padding already provides it.
    PushHeading(0.82f);
    ImGui::PushStyleColor(ImGuiCol_Text, TextFaint());
    ImGui::TextUnformatted(heading);
    ImGui::PopStyleColor();
    PopFont();
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
  }
  return open;
}

void EndCard()
{
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void ToolbarSeparator()
{
  ImGui::SameLine(0.0f, 14.0f);

  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetFrameHeight();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y + 4.0f), ImVec2(at.x, at.y + height - 4.0f),
                                      ImGui::GetColorU32(Line()));
  ImGui::Dummy(ImVec2(1.0f, height));
  ImGui::SameLine(0.0f, 14.0f);
}

void ToolbarGap()
{
  ImGui::SameLine(0.0f, 22.0f);
}

void ViewTitle(const char* text)
{
  PushHeading(1.35f);
  ImGui::TextUnformatted(text);
  PopFont();
}

namespace
{

/// One ring of the mark. Radius and width are fractions of the mark's own radius, read off the
/// drawing rather than guessed at, so the whole thing scales from a favicon to a splash screen.
struct LogoRing
{
  float radius;
  float width;
  bool accent;
};

/// The full mark: three rings, outer and inner in the wood colour, the middle one in the accent.
/// The outermost reaches exactly to the edge - 0.93966 plus half of 0.12069 is 1.
constexpr LogoRing kLogoRings[] = {{0.93966f, 0.12069f, false}, {0.67241f, 0.13793f, true}, {0.37931f, 0.15517f, false}};
constexpr float kLogoHeart = 0.13793f;
/// Half the width of the notch through the rings, as a fraction of the radius. A chord rather than
/// an angle, which is what keeps the notch the same width on every ring instead of opening out.
constexpr float kLogoNotch = 0.095f;

/// The mark for small sizes: one ring and a larger centre. Three rings at sixteen pixels is three
/// grey lines, so at that size the drawing carries one ring at the weight the three had together.
constexpr LogoRing kSmallRings[] = {{0.8427f, 0.31461f, false}};
constexpr float kSmallHeart = 0.33708f;
constexpr float kSmallNotch = 0.1910f;

constexpr float kLogoNotchDegrees = -22.0f;

} // namespace

void Logo(ImDrawList* draw, ImVec2 centre, float size, ImU32 accent, ImU32 wood)
{
  constexpr float kPi = 3.14159265358979323846f;
  const float radius = size * 0.5f;

  // Below this the three rings stop being three rings. Rather than let them silt up into a blur,
  // the mark falls back to the shape drawn for exactly this size.
  const bool small = size < 26.0f;
  const LogoRing* rings = small ? kSmallRings : kLogoRings;
  const int count = small ? 1 : 3;
  const float notch = small ? kSmallNotch : kLogoNotch;
  const float heart = small ? kSmallHeart : kLogoHeart;

  const float middle = kLogoNotchDegrees * kPi / 180.0f;

  for (int i = 0; i < count; i++)
  {
    const float ringRadius = rings[i].radius * radius;
    if (ringRadius <= 0.0f)
      continue;

    // The notch is a fixed width across, so the angle it takes up grows as the rings get smaller -
    // which is what makes it read as one cut through all of them rather than as a wedge.
    const float half = std::asin(std::clamp(notch * radius / ringRadius, 0.0f, 1.0f));

    draw->PathArcTo(centre, ringRadius, middle + half, middle - half + 2.0f * kPi, 0);
    draw->PathStroke(rings[i].accent ? accent : wood, 0, std::max(1.0f, rings[i].width * radius));
  }

  draw->AddCircleFilled(centre, heart * radius, wood, 0);
}

float LogoLockup(ImDrawList* draw, float left, float centreY, float height, ImU32 accent, ImU32 wood)
{
  // Proportions off the horizontal lockup: the name is set at 0.585 of the mark's height and starts
  // 0.36 of it clear of the mark's edge. Held as ratios rather than as the drawing's own numbers so
  // the lockup is the same lockup whatever size it is asked for.
  constexpr float kNameHeight = 0.585f;
  constexpr float kGap = 0.36f;

  Logo(draw, ImVec2(left + height * 0.5f, centreY), height, accent, wood);

  ImFont* font = Heading();
  const float fontSize = height * kNameHeight;
  const char* name = "Woodshed";

  // The drawing places the name on a baseline; ImGui draws from the top. Centring the block on the
  // mark's own line and nudging it up by a fraction of the cap height lands in the same place, and
  // stays right if the face is ever changed.
  const ImVec2 measured = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, name);
  const float x = left + height * (1.0f + kGap);
  draw->AddText(font, fontSize, ImVec2(x, centreY - measured.y * 0.5f - fontSize * 0.04f), wood, name);

  return height * (1.0f + kGap) + measured.x;
}

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 c, float size, ImU32 colour)
{
  const float r = size * 0.5f;
  const float thickness = std::max(1.6f, size * 0.11f);

  // A filled triangle pointing right, from its centre. Every transport glyph is one of these, a
  // bar, or a chevron.
  const auto triangle = [&](float cx, float halfWidth, float halfHeight, bool pointsRight)
  {
    const float tip = pointsRight ? cx + halfWidth : cx - halfWidth;
    const float back = pointsRight ? cx - halfWidth : cx + halfWidth;
    draw->AddTriangleFilled(ImVec2(back, c.y - halfHeight), ImVec2(back, c.y + halfHeight), ImVec2(tip, c.y), colour);
  };
  const auto bar = [&](float cx, float halfWidth, float halfHeight)
  { draw->AddRectFilled(ImVec2(cx - halfWidth, c.y - halfHeight), ImVec2(cx + halfWidth, c.y + halfHeight), colour,
                        halfWidth * 0.5f); };
  const auto chevron = [&](float cx, float w, float h, bool pointsRight)
  {
    const float tip = pointsRight ? cx + w : cx - w;
    const float back = pointsRight ? cx - w : cx + w;
    draw->PathLineTo(ImVec2(back, c.y - h));
    draw->PathLineTo(ImVec2(tip, c.y));
    draw->PathLineTo(ImVec2(back, c.y + h));
    draw->PathStroke(colour, 0, thickness);
  };

  switch (icon)
  {
    case Icon::Play: triangle(c.x + r * 0.08f, r * 0.62f, r * 0.72f, true); break;

    case Icon::Pause:
      bar(c.x - r * 0.30f, r * 0.16f, r * 0.68f);
      bar(c.x + r * 0.30f, r * 0.16f, r * 0.68f);
      break;

    case Icon::Stop:
      draw->AddRectFilled(ImVec2(c.x - r * 0.56f, c.y - r * 0.56f), ImVec2(c.x + r * 0.56f, c.y + r * 0.56f), colour,
                          size * 0.10f);
      break;

    case Icon::SkipStart:
      bar(c.x - r * 0.60f, r * 0.13f, r * 0.66f);
      triangle(c.x + r * 0.14f, r * 0.52f, r * 0.66f, false);
      break;

    case Icon::Rewind:
      triangle(c.x - r * 0.34f, r * 0.44f, r * 0.62f, false);
      triangle(c.x + r * 0.46f, r * 0.44f, r * 0.62f, false);
      break;

    case Icon::Forward:
      triangle(c.x - r * 0.46f, r * 0.44f, r * 0.62f, true);
      triangle(c.x + r * 0.34f, r * 0.44f, r * 0.62f, true);
      break;

    case Icon::Previous: chevron(c.x + r * 0.12f, r * 0.42f, r * 0.52f, false); break;
    case Icon::Next: chevron(c.x - r * 0.12f, r * 0.42f, r * 0.52f, true); break;

    case Icon::Loop:
    {
      // A rounded track with an arrowhead on it - the repeat symbol, drawn rather than borrowed.
      const float w = r * 0.72f;
      const float h = r * 0.48f;
      draw->AddRect(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), colour, h * 0.9f, 0, thickness);
      draw->AddTriangleFilled(ImVec2(c.x + w * 0.18f, c.y - h - thickness * 1.1f),
                              ImVec2(c.x + w * 0.18f, c.y - h + thickness * 1.1f),
                              ImVec2(c.x + w * 0.62f, c.y - h), colour);
      break;
    }

    case Icon::Metronome:
      draw->PathLineTo(ImVec2(c.x - r * 0.62f, c.y + r * 0.68f));
      draw->PathLineTo(ImVec2(c.x - r * 0.20f, c.y - r * 0.70f));
      draw->PathLineTo(ImVec2(c.x + r * 0.20f, c.y - r * 0.70f));
      draw->PathLineTo(ImVec2(c.x + r * 0.62f, c.y + r * 0.68f));
      draw->PathStroke(colour, ImDrawFlags_Closed, thickness);
      draw->AddLine(ImVec2(c.x + r * 0.26f, c.y - r * 0.40f), ImVec2(c.x - r * 0.16f, c.y + r * 0.42f), colour,
                    thickness);
      break;

    case Icon::Plus:
      bar(c.x, r * 0.62f, thickness * 0.5f);
      draw->AddRectFilled(ImVec2(c.x - thickness * 0.5f, c.y - r * 0.62f),
                          ImVec2(c.x + thickness * 0.5f, c.y + r * 0.62f), colour, thickness * 0.25f);
      break;

    case Icon::Minus: bar(c.x, r * 0.62f, thickness * 0.5f); break;

    case Icon::Grid:
      for (int i = -1; i <= 1; i++)
      {
        draw->AddLine(ImVec2(c.x + i * r * 0.5f, c.y - r * 0.66f), ImVec2(c.x + i * r * 0.5f, c.y + r * 0.66f), colour,
                      i == 0 ? thickness : thickness * 0.6f);
      }
      break;

    case Icon::Ruler:
      draw->AddRect(ImVec2(c.x - r * 0.72f, c.y - r * 0.40f), ImVec2(c.x + r * 0.72f, c.y + r * 0.40f), colour,
                    size * 0.08f, 0, thickness);
      for (int i = -1; i <= 1; i++)
        draw->AddLine(ImVec2(c.x + i * r * 0.40f, c.y - r * 0.40f), ImVec2(c.x + i * r * 0.40f, c.y), colour,
                      thickness * 0.7f);
      break;

    case Icon::Eye:
      draw->PathLineTo(ImVec2(c.x - r * 0.76f, c.y));
      draw->PathBezierQuadraticCurveTo(ImVec2(c.x, c.y - r * 0.80f), ImVec2(c.x + r * 0.76f, c.y));
      draw->PathBezierQuadraticCurveTo(ImVec2(c.x, c.y + r * 0.80f), ImVec2(c.x - r * 0.76f, c.y));
      draw->PathStroke(colour, 0, thickness);
      draw->AddCircleFilled(c, r * 0.22f, colour);
      break;

    case Icon::Note:
      // A filled head with a stem and a flag. Small enough that the flag is what makes it read as
      // a note rather than as a pin.
      draw->AddCircleFilled(ImVec2(c.x - r * 0.26f, c.y + r * 0.44f), r * 0.32f, colour, 16);
      draw->AddLine(ImVec2(c.x + r * 0.04f, c.y + r * 0.44f), ImVec2(c.x + r * 0.04f, c.y - r * 0.68f), colour,
                    thickness * 0.9f);
      draw->PathLineTo(ImVec2(c.x + r * 0.04f, c.y - r * 0.68f));
      draw->PathBezierQuadraticCurveTo(ImVec2(c.x + r * 0.62f, c.y - r * 0.52f),
                                       ImVec2(c.x + r * 0.46f, c.y - r * 0.10f));
      draw->PathStroke(colour, 0, thickness * 0.9f);
      break;

    case Icon::Volume:
    {
      // The driver as a small square and the cone as a trapezoid flaring away from it - two convex
      // pieces that meet on a shared edge, so they read as one silhouette. Drawn square rather
      // than rounded: at sixteen pixels a rounded corner is a smudge.
      const float boxLeft = c.x - r * 0.74f;
      const float boxRight = c.x - r * 0.30f;
      const float mouth = c.x + r * 0.14f;

      draw->AddRectFilled(ImVec2(boxLeft, c.y - r * 0.25f), ImVec2(boxRight + 0.5f, c.y + r * 0.25f), colour);
      draw->AddQuadFilled(ImVec2(boxRight, c.y - r * 0.25f), ImVec2(mouth, c.y - r * 0.74f),
                          ImVec2(mouth, c.y + r * 0.74f), ImVec2(boxRight, c.y + r * 0.25f), colour);

      // Two arcs off the mouth. Without them the shape is a flag on a pole.
      for (int i = 0; i < 2; i++)
      {
        const float radius = r * (0.34f + 0.30f * static_cast<float>(i));
        draw->PathArcTo(ImVec2(mouth, c.y), radius, -0.85f, 0.85f, 14);
        draw->PathStroke(colour, 0, thickness * 0.85f);
      }
      break;
    }

    case Icon::Sliders:
      for (int i = 0; i < 3; i++)
      {
        const float y = c.y + (i - 1) * r * 0.52f;
        draw->AddLine(ImVec2(c.x - r * 0.72f, y), ImVec2(c.x + r * 0.72f, y), colour, thickness * 0.7f);
        const float knobX = c.x + ((i % 2 == 0) ? r * 0.28f : -r * 0.30f);
        draw->AddCircleFilled(ImVec2(knobX, y), thickness * 0.95f, colour);
      }
      break;
  }
}

bool SlimSlider(const char* id, float* value, float minValue, float maxValue, float defaultValue, float width,
                const char* format)
{
  constexpr float kHeight = 16.0f;
  constexpr float kTrack = 4.0f;

  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(width, kHeight));
  const bool held = ImGui::IsItemActive();
  const bool over = ImGui::IsItemHovered() || held;

  bool changed = false;

  if (held && width > 0.0f)
  {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - pos.x) / width, 0.0f, 1.0f);
    const float wanted = minValue + t * (maxValue - minValue);
    if (wanted != *value)
    {
      *value = wanted;
      changed = true;
    }
  }

  // Back to where it started, which for a level is unity and for a pan is the middle.
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
  {
    *value = defaultValue;
    changed = true;
  }

  const float span = (maxValue - minValue);
  const float fraction = (span > 0.0f) ? std::clamp((*value - minValue) / span, 0.0f, 1.0f) : 0.0f;
  const float midY = pos.y + kHeight * 0.5f;
  const float at = pos.x + width * fraction;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(pos.x, midY - kTrack * 0.5f), ImVec2(pos.x + width, midY + kTrack * 0.5f),
                      ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)), kTrack * 0.5f);
  draw->AddRectFilled(ImVec2(pos.x, midY - kTrack * 0.5f), ImVec2(at, midY + kTrack * 0.5f),
                      ImGui::GetColorU32(ImVec4(1, 1, 1, over ? 0.85f : 0.55f)), kTrack * 0.5f);
  draw->AddCircleFilled(ImVec2(at, midY), over ? 7.0f : 5.0f,
                        ImGui::GetColorU32(over ? Accent() : ImVec4(1, 1, 1, 0.72f)), 24);

  // The number only while it is wanted, and directly over the handle rather than off beside the
  // pointer. A tooltip sits where the mouse is; this sits where the value is, which is the thing
  // being read - and it stays put while you drag instead of trailing the cursor around.
  //
  // On the foreground list so a bubble that reaches above a narrow row is not clipped away by it.
  if (over)
  {
    char shown[32];
    std::snprintf(shown, sizeof(shown), format, *value);

    const ImVec2 extent = ImGui::CalcTextSize(shown);
    const ImVec2 padding(8.0f, 3.0f);
    const float bubbleWidth = extent.x + padding.x * 2.0f;
    const float bubbleHeight = extent.y + padding.y * 2.0f;

    const ImVec2 from(at - bubbleWidth * 0.5f, midY - 10.0f - bubbleHeight);
    const ImVec2 to(from.x + bubbleWidth, from.y + bubbleHeight);

    ImDrawList* front = ImGui::GetForegroundDrawList();
    front->AddRectFilled(from, to, ImGui::GetColorU32(Control()), bubbleHeight * 0.5f);
    front->AddText(ImVec2(from.x + padding.x, from.y + padding.y), ImGui::GetColorU32(Base()), shown);
  }

  return changed;
}

bool SpinnerButton(const char* id, float size)
{
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 centre(pos.x + size * 0.5f, pos.y + size * 0.5f);
  const float radius = size * 0.36f;
  const float thickness = std::max(2.0f, size * 0.11f);

  if (hovered)
    draw->AddCircleFilled(centre, size * 0.5f, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.09f)), 24);

  // Three quarters of a ring, turning. The gap is what makes it read as motion rather than as a
  // circle that happens to be there.
  const float start = static_cast<float>(ImGui::GetTime()) * 3.2f;
  draw->PathArcTo(centre, radius, start, start + 4.7f, 24);
  draw->PathStroke(ImGui::GetColorU32(hovered ? Control() : Accent()), 0, thickness);

  return clicked;
}

bool SlimSliderVertical(const char* id, float* value, float minValue, float maxValue, float defaultValue, float height,
                        const char* format)
{
  constexpr float kWidth = 16.0f;
  constexpr float kTrack = 4.0f;

  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(kWidth, height));
  const bool held = ImGui::IsItemActive();
  const bool over = ImGui::IsItemHovered() || held;

  bool changed = false;

  if (held && height > 0.0f)
  {
    // Up is more, which is the way every fader ever built runs.
    const float t = 1.0f - std::clamp((ImGui::GetIO().MousePos.y - pos.y) / height, 0.0f, 1.0f);
    const float wanted = minValue + t * (maxValue - minValue);
    if (wanted != *value)
    {
      *value = wanted;
      changed = true;
    }
  }

  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
  {
    *value = defaultValue;
    changed = true;
  }

  const float span = (maxValue - minValue);
  const float fraction = (span > 0.0f) ? std::clamp((*value - minValue) / span, 0.0f, 1.0f) : 0.0f;
  const float midX = pos.x + kWidth * 0.5f;
  const float at = pos.y + height * (1.0f - fraction);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(midX - kTrack * 0.5f, pos.y), ImVec2(midX + kTrack * 0.5f, pos.y + height),
                      ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)), kTrack * 0.5f);
  draw->AddRectFilled(ImVec2(midX - kTrack * 0.5f, at), ImVec2(midX + kTrack * 0.5f, pos.y + height),
                      ImGui::GetColorU32(ImVec4(1, 1, 1, over ? 0.85f : 0.55f)), kTrack * 0.5f);
  draw->AddCircleFilled(ImVec2(midX, at), over ? 7.0f : 5.0f,
                        ImGui::GetColorU32(over ? Accent() : ImVec4(1, 1, 1, 0.72f)), 24);

  if (over)
  {
    char shown[32];
    std::snprintf(shown, sizeof(shown), format, *value);
    const ImVec2 extent = ImGui::CalcTextSize(shown);
    const ImVec2 padding(8.0f, 3.0f);
    const ImVec2 from(midX + 14.0f, at - (extent.y + padding.y * 2.0f) * 0.5f);
    const ImVec2 to(from.x + extent.x + padding.x * 2.0f, from.y + extent.y + padding.y * 2.0f);

    ImDrawList* front = ImGui::GetForegroundDrawList();
    front->AddRectFilled(from, to, ImGui::GetColorU32(Control()), (to.y - from.y) * 0.5f);
    front->AddText(ImVec2(from.x + padding.x, from.y + padding.y), ImGui::GetColorU32(Base()), shown);
  }

  return changed;
}

bool LetterToggle(const char* id, const char* letter, bool* on, float size, ImVec4 lit)
{
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();

  if (clicked && on != nullptr)
    *on = !*on;

  const bool isOn = (on != nullptr) && *on;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 b(pos.x + size, pos.y + size);
  const float rounding = size * 0.26f;

  ImU32 text;
  if (isOn)
  {
    draw->AddRectFilled(pos, b, ImGui::GetColorU32(lit), rounding);
    text = ImGui::GetColorU32(Base()); // dark on the lit square
  }
  else
  {
    if (hovered || held)
      draw->AddRectFilled(pos, b, ImGui::GetColorU32(ImVec4(1, 1, 1, held ? 0.16f : 0.09f)), rounding);
    else
      draw->AddRectFilled(pos, b, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f)), rounding);
    text = ImGui::GetColorU32(hovered ? Control() : TextDim());
  }

  // Centred by measurement rather than by eye, because a letter that is a pixel off centre in a
  // small square is the kind of thing that reads as sloppy without being nameable.
  const ImVec2 extent = ImGui::CalcTextSize(letter);
  draw->AddText(ImVec2(pos.x + (size - extent.x) * 0.5f, pos.y + (size - extent.y) * 0.5f), text, letter);
  return clicked;
}

bool TransportKey(const char* id, Icon icon, ImVec2 size, bool lit, bool active)
{
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 b = ImVec2(pos.x + size.x, pos.y + size.y);
  const float rounding = std::min(8.0f, size.y * 0.24f);

  // One flat fill and nothing else. What tells these apart is size, shape and colour - a bevel or
  // a gradient only says that something is pretending to be a physical object, and at this size
  // the pretence is all you see.
  ImU32 glyph;

  if (lit)
  {
    // The one solid shape on the bar. White on black is the strongest thing this palette can do,
    // so it is spent on the control that gets pressed most and on nothing else.
    draw->AddRectFilled(pos, b, ImGui::GetColorU32(held ? ImVec4(0.82f, 0.82f, 0.85f, 1.0f) : Control()), rounding);
    glyph = ImGui::GetColorU32(Base());
  }
  else if (active)
  {
    // A mode that is armed. This is where the accent earns its keep - it is the only colour on the
    // bar, so it can only mean one thing.
    draw->AddRectFilled(pos, b, ImGui::GetColorU32(AccentDim()), rounding);
    glyph = ImGui::GetColorU32(Accent());
  }
  else
  {
    if (hovered || held)
      draw->AddRectFilled(pos, b, ImGui::GetColorU32(ImVec4(1, 1, 1, held ? 0.16f : 0.09f)), rounding);
    glyph = ImGui::GetColorU32(hovered ? Control() : TextDim());
  }

  const ImVec2 centre(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
  DrawIcon(draw, icon, centre, std::min(size.x, size.y) * 0.62f, glyph);
  return clicked;
}

bool IconButton(const char* id, Icon icon, float size, IconStyle style, bool active)
{
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 centre(pos.x + size * 0.5f, pos.y + size * 0.5f);
  const float radius = size * 0.5f;

  ImU32 glyph = ImGui::GetColorU32(Control());

  // Rounded squares rather than discs, and one flat fill each - the same vocabulary the transport
  // keys use, so the two do not read as two different kits.
  const ImVec2 a(pos.x, pos.y);
  const ImVec2 b(pos.x + size, pos.y + size);
  const float rounding = size * 0.26f;
  (void)radius;

  if (style == IconStyle::Primary)
  {
    draw->AddRectFilled(a, b, ImGui::GetColorU32(held ? ImVec4(0.82f, 0.82f, 0.85f, 1.0f) : Control()), rounding);
    glyph = ImGui::GetColorU32(Base());
  }
  else if (active)
  {
    draw->AddRectFilled(a, b, ImGui::GetColorU32(AccentDim()), rounding);
    glyph = ImGui::GetColorU32(Accent());
  }
  else
  {
    if (hovered || held)
      draw->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(1, 1, 1, held ? 0.16f : 0.09f)), rounding);
    glyph = ImGui::GetColorU32(hovered ? Control() : TextDim());
  }

  DrawIcon(draw, icon, centre, size * 0.58f, glyph);
  return clicked;
}

bool InInputTargetZone(float levelDb)
{
  return levelDb >= kInputTargetLowDb && levelDb <= kInputTargetHighDb;
}

void BigMeter(float levelDb, float width, float height, bool showTargetZone)
{
  // Shared -60 dB floor with LevelMeter, so the two read consistently.
  constexpr float kFloorDb = -60.0f;
  const auto toX = [&](float db) { return std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f); };
  const float normalized = toX(levelDb);

  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 end = ImVec2(start.x + width, start.y + height);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float rounding = height * 0.25f;

  draw->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);

  if (showTargetZone)
  {
    const float zoneStart = start.x + width * toX(kInputTargetLowDb);
    const float zoneEnd = start.x + width * toX(kInputTargetHighDb);
    const ImVec4 zone = Success();
    draw->AddRectFilled(ImVec2(zoneStart, start.y), ImVec2(zoneEnd, end.y),
                        ImGui::GetColorU32(ImVec4(zone.x, zone.y, zone.z, 0.16f)));
    // Edges of the band, so the target reads even when the meter is empty.
    const ImU32 edge = ImGui::GetColorU32(ImVec4(zone.x, zone.y, zone.z, 0.55f));
    draw->AddLine(ImVec2(zoneStart, start.y), ImVec2(zoneStart, end.y), edge, 1.5f);
    draw->AddLine(ImVec2(zoneEnd, start.y), ImVec2(zoneEnd, end.y), edge, 1.5f);
  }

  ImVec4 fill = Accent();
  if (levelDb > -3.0f)
    fill = Danger();
  else if (showTargetZone && InInputTargetZone(levelDb))
    fill = Success();

  if (normalized > 0.001f)
  {
    draw->AddRectFilled(start, ImVec2(start.x + width * normalized, end.y), ImGui::GetColorU32(fill), rounding);
  }

  ImGui::Dummy(ImVec2(width, height));
}

void VerticalMeter(float levelDb, float width, float height, bool showTargetZone)
{
  constexpr float kFloorDb = -60.0f;
  const auto toFraction = [&](float db) { return std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f); };
  const float normalized = toFraction(levelDb);

  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 end = ImVec2(start.x + width, start.y + height);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float rounding = width * 0.3f;

  draw->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);

  if (showTargetZone)
  {
    // Screen y grows downward, so the louder edge of the band is the smaller y.
    const float zoneTop = end.y - height * toFraction(kInputTargetHighDb);
    const float zoneBottom = end.y - height * toFraction(kInputTargetLowDb);
    const ImVec4 zone = Success();
    draw->AddRectFilled(ImVec2(start.x, zoneTop), ImVec2(end.x, zoneBottom),
                        ImGui::GetColorU32(ImVec4(zone.x, zone.y, zone.z, 0.16f)));
    const ImU32 edge = ImGui::GetColorU32(ImVec4(zone.x, zone.y, zone.z, 0.55f));
    draw->AddLine(ImVec2(start.x, zoneTop), ImVec2(end.x, zoneTop), edge, 1.5f);
    draw->AddLine(ImVec2(start.x, zoneBottom), ImVec2(end.x, zoneBottom), edge, 1.5f);
  }

  ImVec4 fill = Accent();
  if (levelDb > -3.0f)
    fill = Danger();
  else if (showTargetZone && InInputTargetZone(levelDb))
    fill = Success();

  if (normalized > 0.001f)
    draw->AddRectFilled(ImVec2(start.x, end.y - height * normalized), end, ImGui::GetColorU32(fill), rounding);

  ImGui::Dummy(ImVec2(width, height));
}

namespace
{
// What a detented knob's value would be without the snapping. Only one knob can be dragged at a
// time, so one slot is all this needs.
ImGuiID gDetentKnobId = 0;
float gDetentFreeValue = 0.0f;
} // namespace

ImVec4 LevelColour(float levelDb)
{
  // Green where there is room to spare, amber where it should sit, red as it runs out. The two
  // ends are the ones that matter; the middle is the app's own colour, which is what makes "right"
  // look like the rest of the application rather than like a warning that has not gone off yet.
  static const ImVec4 kQuiet(0.42f, 0.78f, 0.50f, 1.0f);
  static const ImVec4 kGood = Accent();
  static const ImVec4 kHot(0.92f, 0.33f, 0.31f, 1.0f);

  const auto mix = [](const ImVec4& a, const ImVec4& b, float t)
  {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
  };

  if (levelDb <= kInputTargetLowDb)
  {
    // -40 dB and below is as green as it gets; from there it warms up to the bottom of the band.
    const float t = std::clamp((levelDb + 40.0f) / (kInputTargetLowDb + 40.0f), 0.0f, 1.0f);
    return mix(kQuiet, kGood, t);
  }

  // Above the band it reddens, and is fully red a few dB short of clipping rather than at it -
  // by the time a meter says you have clipped it is too late to have not.
  const float t = std::clamp((levelDb - kInputTargetHighDb) / (-1.0f - kInputTargetHighDb), 0.0f, 1.0f);
  return mix(kGood, kHot, t);
}

bool Knob(const char* label, float* value, float minValue, float maxValue, float defaultValue, const char* format,
          float diameter, const float* detents, int detentCount, bool showName, const ImVec4* arcColour)
{
  constexpr float kPi = 3.14159265358979323846f;
  // A 270-degree sweep starting down-left and ending down-right, the way a real control is marked.
  constexpr float kAngleMin = 0.75f * kPi;
  constexpr float kAngleMax = 2.25f * kPi;
  // How far the mouse travels for the full range. Generous, so the knob is easy to place.
  constexpr float kDragPixels = 260.0f;

  ImGui::PushID(label);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float textHeight = ImGui::GetTextLineHeight();
  // Without its name the knob is a line shorter, which is the difference between three of them
  // fitting down a narrow rail and the last one being cut off.
  const float totalHeight = diameter + textHeight * (showName ? 2.0f : 1.0f) + 6.0f;

  ImGui::InvisibleButton("knob", ImVec2(diameter, totalHeight));

  bool changed = false;
  const float span = maxValue - minValue;

  const bool hasDetents = (detents != nullptr && detentCount > 0);
  if (hasDetents && ImGui::IsItemActivated())
  {
    gDetentKnobId = ImGui::GetItemID();
    gDetentFreeValue = *value;
  }

  if (ImGui::IsItemActive() && span > 0.0f)
  {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.y != 0.0f)
    {
      const float sensitivity = io.KeyShift ? 0.25f : 1.0f;
      // Up increases: dragging away from you turns it up, which is what the hand expects.
      const float delta = -(io.MouseDelta.y / kDragPixels) * span * sensitivity;

      if (hasDetents && gDetentKnobId == ImGui::GetItemID())
      {
        // The unsnapped value keeps following the mouse while the reported one sits on the detent.
        // That is what makes leaving one take a deliberate pull rather than a twitch, and what
        // stops the knob sticking the moment you try to drag away.
        constexpr float kDetentPixels = 9.0f;
        const float window = span * (kDetentPixels / kDragPixels) * sensitivity;

        gDetentFreeValue = std::clamp(gDetentFreeValue + delta, minValue, maxValue);
        *value = gDetentFreeValue;
        for (int i = 0; i < detentCount; i++)
          if (std::fabs(gDetentFreeValue - detents[i]) < window)
            *value = detents[i];
      }
      else
      {
        *value = std::clamp(*value + delta, minValue, maxValue);
      }
      changed = true;
    }
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
  {
    *value = std::clamp(defaultValue, minValue, maxValue);
    changed = true;
  }

  const float normalized = span > 0.0f ? std::clamp((*value - minValue) / span, 0.0f, 1.0f) : 0.0f;
  const float angle = kAngleMin + normalized * (kAngleMax - kAngleMin);

  const float radius = diameter * 0.5f;
  const ImVec2 center = ImVec2(origin.x + radius, origin.y + radius);
  ImDrawList* draw = ImGui::GetWindowDrawList();

  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  // The arc is the value, so a caller with something better to say with it - a level, say - hands
  // its own colour in and the knob becomes its own meter.
  const ImVec4 accent = (arcColour != nullptr) ? *arcColour : Accent();
  const float trackThickness = std::max(3.0f, diameter * 0.075f);
  const float trackRadius = radius - trackThickness * 0.5f;

  // Track
  draw->PathArcTo(center, trackRadius, kAngleMin, kAngleMax, 48);
  draw->PathStroke(ImGui::GetColorU32(ImGuiCol_FrameBg), 0, trackThickness);

  // Value arc. A range that straddles zero fills out from the centre instead of from the left,
  // so a cut reads as a cut rather than as "less boost".
  const bool bipolar = (minValue < 0.0f && maxValue > 0.0f);
  const float zeroNormalized = bipolar ? (-minValue / span) : 0.0f;
  const float zeroAngle = kAngleMin + zeroNormalized * (kAngleMax - kAngleMin);
  if (std::fabs(angle - zeroAngle) > 0.001f)
  {
    draw->PathArcTo(center, trackRadius, std::min(zeroAngle, angle), std::max(zeroAngle, angle), 48);
    draw->PathStroke(ImGui::GetColorU32(accent), 0, trackThickness);
  }

  // The control itself is white; the accent is reserved for the value it is showing.
  const float bodyRadius = trackRadius - trackThickness * 0.9f;
  draw->AddCircle(center, bodyRadius,
                  ImGui::GetColorU32(hot ? Control() : ImGui::GetStyle().Colors[ImGuiCol_Border]), 48,
                  hot ? 1.6f : 1.0f);

  const ImVec2 direction = ImVec2(std::cos(angle), std::sin(angle));
  draw->AddLine(ImVec2(center.x + direction.x * bodyRadius * 0.30f, center.y + direction.y * bodyRadius * 0.30f),
                ImVec2(center.x + direction.x * bodyRadius * 0.90f, center.y + direction.y * bodyRadius * 0.90f),
                ImGui::GetColorU32(Control()), std::max(2.0f, diameter * 0.05f));

  // Value, then label, both centred under the knob.
  char valueText[64];
  std::snprintf(valueText, sizeof(valueText), format, *value);

  const auto centredText = [&](const char* text, float y, ImU32 color)
  {
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(center.x - size.x * 0.5f, y), color, text);
  };
  centredText(valueText, origin.y + diameter + 2.0f, ImGui::GetColorU32(ImGuiCol_Text));
  if (showName)
    centredText(label, origin.y + diameter + 4.0f + textHeight, ImGui::GetColorU32(TextDim()));
  else if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", label);

  ImGui::PopID();
  return changed;
}

bool PowerButton(const char* id, bool* on, float size)
{
  constexpr float kPi = 3.14159265358979323846f;

  ImGui::PushID(id);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("power", ImVec2(size, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  if (clicked)
    *on = !*on;

  const ImVec2 center = ImVec2(origin.x + size * 0.5f, origin.y + size * 0.5f);
  const float radius = size * 0.34f;
  const float thickness = std::max(1.8f, size * 0.09f);
  const ImVec4 tint = *on ? Accent() : (hovered ? Control() : TextDim());
  const ImU32 color = ImGui::GetColorU32(tint);

  ImDrawList* draw = ImGui::GetWindowDrawList();

  // A ring with a gap at the top, and a stroke rising through it.
  constexpr float kGap = 0.62f; // radians either side of vertical
  draw->PathArcTo(center, radius, -kPi * 0.5f + kGap, kPi * 1.5f - kGap, 32);
  draw->PathStroke(color, 0, thickness);
  draw->AddLine(ImVec2(center.x, center.y - radius * 1.15f), ImVec2(center.x, center.y - radius * 0.15f), color,
                thickness);

  return clicked;
}

namespace
{
constexpr float kChipPadX = 6.0f;
constexpr float kChipPadY = 1.0f;
} // namespace

void Spinner(float size)
{
  constexpr float kPi = 3.14159265358979323846f;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 center = ImVec2(origin.x + size * 0.5f, origin.y + size * 0.5f);
  const float radius = size * 0.34f;
  const float thickness = std::max(1.8f, size * 0.10f);

  // A three-quarter arc whose start walks round the circle. Two turns a second reads as working
  // without becoming the busiest thing on screen.
  const float phase = static_cast<float>(ImGui::GetTime()) * kPi;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->PathArcTo(center, radius, phase, phase + kPi * 1.5f, 24);
  draw->PathStroke(ImGui::GetColorU32(Accent()), 0, thickness);

  ImGui::Dummy(ImVec2(size, size));
}

float ChipWidth(const char* text)
{
  return ImGui::CalcTextSize(text).x + kChipPadX * 2.0f;
}

void Chip(const char* text, ImVec4 color)
{
  const ImVec2 textSize = ImGui::CalcTextSize(text);
  const ImVec2 size = ImVec2(textSize.x + kChipPadX * 2.0f, textSize.y + kChipPadY * 2.0f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.16f)), 3.0f);
  draw->AddText(ImVec2(origin.x + kChipPadX, origin.y + kChipPadY), ImGui::GetColorU32(color), text);

  ImGui::Dummy(size);
}

bool Check(const char* label, bool* value)
{
  const float box = ImGui::GetFontSize() + 2.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);

  ImGui::PushID(label);
  // The label is part of the target: a four-pixel square is not something to ask anyone to hit.
  ImGui::InvisibleButton("hit", ImVec2(box + 8.0f + labelSize.x, std::max(box, labelSize.y)));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  if (clicked)
    *value = !*value;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 boxMin(origin.x, origin.y + (std::max(box, labelSize.y) - box) * 0.5f);
  const ImVec2 boxMax(boxMin.x + box, boxMin.y + box);

  const ImU32 line = ImGui::GetColorU32(*value ? Control() : (hovered ? TextDim() : Line()));
  if (hovered)
    draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)), 3.0f);
  draw->AddRect(boxMin, boxMax, line, 3.0f, 0, 1.5f);

  // A tick, drawn: two strokes, the short one down into the corner and the long one back up.
  if (*value)
  {
    const float w = box;
    draw->PathLineTo(ImVec2(boxMin.x + w * 0.24f, boxMin.y + w * 0.52f));
    draw->PathLineTo(ImVec2(boxMin.x + w * 0.44f, boxMin.y + w * 0.72f));
    draw->PathLineTo(ImVec2(boxMin.x + w * 0.78f, boxMin.y + w * 0.28f));
    draw->PathStroke(ImGui::GetColorU32(Control()), 0, 2.0f);
  }

  if (labelSize.x > 0.0f)
    draw->AddText(ImVec2(boxMax.x + 8.0f, origin.y + (std::max(box, labelSize.y) - labelSize.y) * 0.5f),
                  ImGui::GetColorU32(*value ? Text() : TextDim()), label);

  return clicked;
}

bool CloseButton(const char* id, float size)
{
  ImGui::PushID(id);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("close", ImVec2(size, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  const ImVec2 center = ImVec2(origin.x + size * 0.5f, origin.y + size * 0.5f);
  const float arm = size * 0.26f;
  const float thickness = std::max(1.6f, size * 0.09f);
  const ImU32 color = ImGui::GetColorU32(hovered ? Danger() : Control());

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddLine(ImVec2(center.x - arm, center.y - arm), ImVec2(center.x + arm, center.y + arm), color, thickness);
  draw->AddLine(ImVec2(center.x - arm, center.y + arm), ImVec2(center.x + arm, center.y - arm), color, thickness);

  return clicked;
}

bool StarButton(const char* id, bool filled, float size)
{
  constexpr float kPi = 3.14159265358979323846f;

  ImGui::PushID(id);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("star", ImVec2(size, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  const ImVec2 center = ImVec2(origin.x + size * 0.5f, origin.y + size * 0.5f);
  const float outer = size * 0.34f;
  const float inner = outer * 0.42f;
  const ImU32 color = ImGui::GetColorU32(filled ? Accent() : (hovered ? Control() : TextDim()));

  ImDrawList* draw = ImGui::GetWindowDrawList();

  // Ten points, alternating outer and inner radius, starting at the top.
  ImVec2 points[10];
  for (int i = 0; i < 10; i++)
  {
    const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / 5.0f;
    const float radius = (i % 2 == 0) ? outer : inner;
    points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
  }

  if (filled)
  {
    // A star is concave, so PathFillConvex would render it wrong; fan triangles from the centre.
    for (int i = 0; i < 10; i++)
      draw->AddTriangleFilled(center, points[i], points[(i + 1) % 10], color);
  }
  else
  {
    for (int i = 0; i < 10; i++)
      draw->PathLineTo(points[i]);
    draw->PathStroke(color, ImDrawFlags_Closed, 1.6f);
  }

  return clicked;
}

bool KnobReleased()
{
  // Not IsItemDeactivatedAfterEdit(): that needs the item to have been marked edited, which an
  // InvisibleButton never is. Losing active state is the same moment for a knob.
  return ImGui::IsItemDeactivated();
}

void NeedleGauge(float cents, bool hasSignal, bool inTune, float width, float height)
{
  constexpr float kPi = 3.14159265358979323846f;
  // A 120-degree fan. Wider than that and the needle stops reading as a needle; narrower and the
  // last few cents are too cramped to see.
  constexpr float kHalfSweep = kPi / 3.0f;
  constexpr float kFullScaleCents = 50.0f;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  const ImVec2 pivot = ImVec2(origin.x + width * 0.5f, origin.y + height * 0.92f);
  const float radius = std::min(width * 0.46f, height * 0.84f);

  // Straight up is in tune; the scale opens out either side of it.
  const auto angleFor = [&](float c)
  { return -kPi * 0.5f + std::clamp(c / kFullScaleCents, -1.0f, 1.0f) * kHalfSweep; };

  const auto pointAt = [&](float angle, float distance)
  { return ImVec2(pivot.x + std::cos(angle) * distance, pivot.y + std::sin(angle) * distance); };

  // The scale, then the band that counts as in tune drawn over it.
  draw->PathArcTo(pivot, radius, angleFor(-kFullScaleCents), angleFor(kFullScaleCents), 64);
  draw->PathStroke(ImGui::GetColorU32(ImGuiCol_FrameBg), 0, 4.0f);

  draw->PathArcTo(pivot, radius, angleFor(-3.0f), angleFor(3.0f), 12);
  draw->PathStroke(ImGui::GetColorU32(Success()), 0, 4.0f);

  for (int tick = -2; tick <= 2; tick++)
  {
    const float c = static_cast<float>(tick) * 25.0f;
    const float angle = angleFor(c);
    const bool major = (tick == 0);
    draw->AddLine(pointAt(angle, radius * (major ? 0.78f : 0.86f)), pointAt(angle, radius),
                  ImGui::GetColorU32(major ? Control() : TextDim()), major ? 2.0f : 1.4f);
  }

  // With nothing playing the needle rests at centre rather than wherever it was left, which would
  // read as a note being in tune when there is no note at all.
  const float shown = hasSignal ? cents : 0.0f;
  const float angle = angleFor(shown);
  const ImVec4 needleColor = !hasSignal ? TextDim() : (inTune ? Success() : Accent());

  draw->AddLine(pivot, pointAt(angle, radius * 0.94f), ImGui::GetColorU32(needleColor), 3.0f);
  draw->AddCircleFilled(pivot, 5.0f, ImGui::GetColorU32(needleColor), 16);

  // Flat and sharp, at the ends where the needle points when you are out. Centred on the point
  // rather than hung off its top-left corner, which is where AddText would otherwise put them.
  const ImU32 dim = ImGui::GetColorU32(TextDim());
  const auto label = [&](float c, const char* text)
  {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at = pointAt(angleFor(c), radius * 1.10f);
    draw->AddText(ImVec2(at.x - size.x * 0.5f, at.y - size.y * 0.5f), dim, text);
  };
  label(-kFullScaleCents, "b");
  label(kFullScaleCents, "#");

  ImGui::Dummy(ImVec2(width, height));
}

void StrobeStrip(float phase, float strength, float width, float height, int blocks, bool inTune)
{
  constexpr float kTwoPi = 6.2831853071795864769f;

  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 end = ImVec2(start.x + width, start.y + height);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float rounding = 4.0f;

  draw->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);

  const int pairs = std::max(1, blocks);
  const float pairWidth = width / static_cast<float>(pairs);
  const float blockWidth = pairWidth * 0.5f;

  // One full turn of phase moves the pattern by exactly one dark/light pair, so a 1 Hz error
  // scrolls the strip one pair per second no matter which band it is.
  float offset = std::fmod(phase / kTwoPi, 1.0f) * pairWidth;
  if (offset < 0.0f)
    offset += pairWidth;

  const ImVec4 base = inTune ? Success() : Accent();
  const float alpha = 0.25f + 0.75f * std::clamp(strength, 0.0f, 1.0f);
  const ImU32 color = ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, alpha));

  draw->PushClipRect(start, end, true);
  // One extra pair so the block scrolling in from the left is always drawn.
  for (int i = -1; i <= pairs; i++)
  {
    const float x = start.x + offset + static_cast<float>(i) * pairWidth;
    draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + blockWidth, end.y), color);
  }
  draw->PopClipRect();

  draw->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border), rounding);
  ImGui::Dummy(ImVec2(width, height));
}

bool MenuButton(const char* label, float sizeScale, float minWidth)
{
  PushHeading(sizeScale);

  // ### hides everything after it, so a caller passing "Bar###preset" gets a stable id while the
  // visible half changes. CalcTextSize has to be told the same, or the button is sized for the id.
  const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
  constexpr float kCaretRoom = 22.0f;
  const ImVec2 size(std::max(minWidth, textSize.x + ImGui::GetStyle().FramePadding.x * 2.0f + kCaretRoom),
                    textSize.y + ImGui::GetStyle().FramePadding.y * 2.0f);

  // Left-aligned, so the room kept for the caret does not push the name off centre.
  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  PopFont();

  const ImVec2 from = ImGui::GetItemRectMin();
  const ImVec2 to = ImGui::GetItemRectMax();
  const float cx = to.x - kCaretRoom * 0.5f - 2.0f;
  const float cy = (from.y + to.y) * 0.5f + 1.0f;
  const ImU32 colour = ImGui::GetColorU32(ImGui::IsItemHovered() ? Text() : TextDim());
  ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - 4.5f, cy - 2.5f), ImVec2(cx + 4.5f, cy - 2.5f),
                                                ImVec2(cx, cy + 3.0f), colour);
  return pressed;
}

void LevelMeter(const char* id, float levelDb, float width)
{
  // -60 dB floor: below that the signal is inaudible in this context and the bar may as well
  // read empty rather than wobbling around at the far left.
  const float normalized = std::clamp((levelDb + 60.0f) / 60.0f, 0.0f, 1.0f);

  ImVec4 fill = Success();
  if (levelDb > -3.0f)
    fill = Danger();
  else if (levelDb > -12.0f)
    fill = Warning();

  const float height = ImGui::GetFrameHeight() * 0.55f;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float rounding = height * 0.5f;

  draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
  if (normalized > 0.001f)
  {
    draw->AddRectFilled(start, ImVec2(start.x + width * normalized, start.y + height), ImGui::GetColorU32(fill),
                        rounding);
  }

  ImGui::Dummy(ImVec2(width, height));

  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, TextDim());
  if (levelDb <= -99.0f)
    ImGui::TextUnformatted("-inf dB");
  else
    ImGui::Text("%.0f dB", levelDb);
  ImGui::PopStyleColor();
  IM_UNUSED(id);
}

} // namespace nam_ui::theme
