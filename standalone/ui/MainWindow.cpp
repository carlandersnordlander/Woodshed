#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_set>

#include "imgui.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "AudioFile.h"
#include "OAuth.h"
#include "Tone3000AppKey.h"
#include "SecretStore.h"
#include "Theme.h"
#include "Tuner.h"
#include "Win32FileDialog.h"

namespace nam_ui
{

namespace
{

/// Output gain target used when matching capture loudness, so comparisons are about tone.
constexpr float kTargetLoudnessDb = -18.0f;
constexpr unsigned int kCommonSampleRates[] = {44100, 48000, 88200, 96000};

/// Vertical filter column in the two browsing views.
constexpr float kFilterPanelWidth = 280.0f;

/// The numbered strip along the top of the timeline. Loops live in it as well as over the
/// waveforms, so it is part of the timeline's hit area rather than a separate widget.
constexpr float kRulerHeight = 34.0f;

/// How wide the navigation column is. Narrow on purpose: it holds six glyphs and nothing else.
constexpr float kNavRailWidth = 54.0f;

/// The keys, the scrub bar under them, and air. Reserved at the bottom of every view, because the
/// transport belongs to the application rather than to the player: wanting the song running while
/// you set up an amp, tune, or pick a capture is the normal case, not an exception.
constexpr float kTransportHeight = 36.0f + 12.0f + 16.0f + 12.0f;

/// How thick the hand-drawn scrollbars are.
constexpr float kScrollBarThickness = 12.0f;

/// Lanes are told apart by where they are and what they are called, not by being different
/// colours. White at two strengths says the one thing that matters about a waveform - whether it
/// is going to be heard - and leaves the accent free to mean something.
constexpr float kWaveAudible = 0.62f;
constexpr float kWaveSilent = 0.16f;

/// What part of a zoom scrollbar's thumb a drag took hold of.
enum class ScrollGrab
{
  None,
  Start,  ///< the near end - moves it, holding the far end still
  Middle, ///< the body - moves the window without resizing it
  End     ///< the far end
};

ScrollGrab gScrollGrab = ScrollGrab::None;
ImGuiID gScrollGrabId = 0;
/// For a middle drag, how far into the window it was grabbed. For an end drag, the edge that is
/// staying put. Both in fractions of the content.
float gScrollGrabAnchor = 0.0f;

/// \brief A scrollbar that is also the zoom: drag the middle to move, drag either end to change
/// how much is shown.
///
/// One control for both jobs, because they are the same question asked twice - which part of the
/// content am I looking at, and how much of it. A separate zoom means two things to reach for and
/// no way to see the relationship between them; here the thumb *is* the view, and its length is
/// how much you can see.
///
/// Drawn by hand rather than left to ImGui for a second reason: the track controls and the
/// waveforms are two children that have to move as one, and a scrollbar each is one place for them
/// to come apart.
///
/// Everything is in fractions of the content, so the same function serves time along the bottom
/// and lane height down the side without either caller having to think in the other's units.
///
/// \param start in/out where the window begins, 0..1
/// \param size  in/out how much of the content it covers, 0..1
/// \return true when either changed
bool ZoomScrollBar(const char* id, ImVec2 topLeft, float length, bool vertical, float& start, float& size,
                   float minSize)
{
  if (length < 16.0f)
    return false;

  const ImVec2 extent = vertical ? ImVec2(kScrollBarThickness, length) : ImVec2(length, kScrollBarThickness);

  ImGui::SetCursorScreenPos(topLeft);
  ImGui::InvisibleButton(id, extent);
  const ImGuiID itemId = ImGui::GetItemID();
  const bool held = ImGui::IsItemActive();
  const bool hovered = ImGui::IsItemHovered() || held;

  size = std::clamp(size, minSize, 1.0f);
  start = std::clamp(start, 0.0f, 1.0f - size);

  // Not called "near": windows.h still defines that as a macro, and has since 16-bit memory models.
  const float axisOrigin = vertical ? topLeft.y : topLeft.x;
  const auto pointerFraction = [&]()
  {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float along = (vertical ? mouse.y : mouse.x) - axisOrigin;
    return std::clamp(along / length, 0.0f, 1.0f);
  };

  // The thumb never shrinks below something you can actually take hold of, so its ends stay
  // grabbable however far in you have zoomed.
  constexpr float kMinThumb = 30.0f;
  const float thumbLength = std::max(kMinThumb, length * size);
  const float thumbStart = (size < 1.0f) ? (start / (1.0f - size)) * (length - thumbLength) : 0.0f;

  constexpr float kEdgeGrab = 9.0f;
  bool changed = false;

  if (ImGui::IsItemActivated())
  {
    const float at = pointerFraction() * length;
    gScrollGrabId = itemId;

    if (at < thumbStart - 2.0f || at > thumbStart + thumbLength + 2.0f)
    {
      // Outside the thumb: jump there, then carry on as a move.
      gScrollGrab = ScrollGrab::Middle;
      gScrollGrabAnchor = size * 0.5f;
    }
    else if (at <= thumbStart + kEdgeGrab)
    {
      gScrollGrab = ScrollGrab::Start;
      gScrollGrabAnchor = start + size; // the far edge holds still
    }
    else if (at >= thumbStart + thumbLength - kEdgeGrab)
    {
      gScrollGrab = ScrollGrab::End;
      gScrollGrabAnchor = start; // the near edge holds still
    }
    else
    {
      gScrollGrab = ScrollGrab::Middle;
      gScrollGrabAnchor = pointerFraction() - start;
    }
  }

  if (held && gScrollGrabId == itemId)
  {
    const float at = pointerFraction();
    switch (gScrollGrab)
    {
      case ScrollGrab::Start:
      {
        const float fixedEnd = gScrollGrabAnchor;
        const float wanted = std::clamp(at, 0.0f, fixedEnd - minSize);
        start = wanted;
        size = fixedEnd - wanted;
        changed = true;
        break;
      }
      case ScrollGrab::End:
      {
        const float fixedStart = gScrollGrabAnchor;
        const float wanted = std::clamp(at, fixedStart + minSize, 1.0f);
        start = fixedStart;
        size = wanted - fixedStart;
        changed = true;
        break;
      }
      case ScrollGrab::Middle:
      {
        start = std::clamp(at - gScrollGrabAnchor, 0.0f, 1.0f - size);
        changed = true;
        break;
      }
      default: break;
    }
  }

  if (!held && gScrollGrabId == itemId)
  {
    gScrollGrab = ScrollGrab::None;
    gScrollGrabId = 0;
  }

  // --- drawn ---
  size = std::clamp(size, minSize, 1.0f);
  start = std::clamp(start, 0.0f, 1.0f - size);

  const float drawnLength = std::max(kMinThumb, length * size);
  const float drawnStart = (size < 1.0f) ? (start / (1.0f - size)) * (length - drawnLength) : 0.0f;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 bottomRight(topLeft.x + extent.x, topLeft.y + extent.y);
  draw->AddRectFilled(topLeft, bottomRight, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.04f)), kScrollBarThickness * 0.5f);

  const ImVec2 thumbFrom = vertical ? ImVec2(topLeft.x + 2.0f, topLeft.y + drawnStart)
                                    : ImVec2(topLeft.x + drawnStart, topLeft.y + 2.0f);
  const ImVec2 thumbTo = vertical ? ImVec2(bottomRight.x - 2.0f, topLeft.y + drawnStart + drawnLength)
                                  : ImVec2(topLeft.x + drawnStart + drawnLength, bottomRight.y - 2.0f);

  draw->AddRectFilled(thumbFrom, thumbTo, ImGui::GetColorU32(ImVec4(1, 1, 1, hovered ? 0.34f : 0.20f)),
                      kScrollBarThickness * 0.5f);

  // Two marks at the ends of the thumb, so it is visible that they are the part you pull to zoom.
  if (hovered)
  {
    const ImU32 grip = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.65f));
    if (vertical)
    {
      const float x0 = topLeft.x + 3.5f;
      const float x1 = bottomRight.x - 3.5f;
      draw->AddLine(ImVec2(x0, thumbFrom.y + 3.5f), ImVec2(x1, thumbFrom.y + 3.5f), grip, 1.5f);
      draw->AddLine(ImVec2(x0, thumbTo.y - 3.5f), ImVec2(x1, thumbTo.y - 3.5f), grip, 1.5f);
    }
    else
    {
      const float y0 = topLeft.y + 3.5f;
      const float y1 = bottomRight.y - 3.5f;
      draw->AddLine(ImVec2(thumbFrom.x + 3.5f, y0), ImVec2(thumbFrom.x + 3.5f, y1), grip, 1.5f);
      draw->AddLine(ImVec2(thumbTo.x - 3.5f, y0), ImVec2(thumbTo.x - 3.5f, y1), grip, 1.5f);
    }
  }

  if (hovered)
    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);

  return changed;
}

const char* BlockTypeName(BlockType type)
{
  switch (type)
  {
    case BlockType::Ir: return "IR";
    case BlockType::Cut: return "CUT";
    case BlockType::Comp: return "COMP";
    case BlockType::Eq: return "EQ";
    default: return "NAM";
  }
}

const char* BlockTypeHint(BlockType type)
{
  return type == BlockType::Ir ? "cab impulse response (.wav)" : "neural capture (.nam)";
}

/// \brief What a block is drawn in - white, whatever kind it is.
///
/// This used to be a colour per type, on the reasoning that a signal path should read by category
/// the way a Helix grid does. But every block already says what it is across its face, so the hue
/// was a second copy of the label - and five of them made the chain the most colourful thing in an
/// application that is otherwise black and white. The accent is worth more kept for the one block
/// you have selected.
ImVec4 BlockColor(BlockType type)
{
  (void)type;
  return theme::Control();
}

struct FilterOption
{
  const char* label;
  const char* value;
};

const FilterOption kGearOptions[] = {{"Amp", "amp"},           {"Amp + cab", "amp-cab"}, {"Pedal", "pedal"},
                                     {"Outboard", "outboard"}, {"Cab", "cab"},           {"Space", "space"},
                                     {"Experimental", "experimental"}};

const FilterOption kSizeOptions[] = {
  {"Standard", "standard"}, {"Lite", "lite"}, {"Feather", "feather"}, {"Nano", "nano"}, {"Custom", "custom"}};

bool VectorContains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Stable ids for the sortable columns, so the sort comparators do not depend on column order.
constexpr ImGuiID kLibColumnName = 1;
constexpr ImGuiID kLibColumnType = 2;
constexpr ImGuiID kLibColumnCreator = 3;
constexpr ImGuiID kCloudColumnDownloads = 4;

/// One string of a tuning, low to high. Note indices are 0 = C, octaves scientific.
struct TunedString
{
  int noteIndex;
  int octave;
};

/// \brief A tuning, as a family and what it is in.
///
/// Which is how they are named and how they are chosen: nobody looks for "the tuning whose strings
/// are D A D G B E", they look for "drop, in D". Splitting the two makes a list of thirty
/// combinations into two rows of buttons, and makes the ones that do not exist impossible to ask
/// for rather than merely absent.
struct TuningPreset
{
  const char* type; ///< "Standard", "Drop", "Open", "DADGAD"
  const char* key;  ///< what it is in - the chord for open tunings, the lowest string otherwise
  int instrument;   ///< 0 = guitar, 1 = bass
  int stringCount;
  TunedString strings[8];

  /// How it is written down and stored: "Drop D", "Open G".
  std::string Name() const { return std::string(type) + " " + key; }
};

/// A family of tunings: one shape, in every key. `intervals` are semitones above the lowest string,
/// so one row here is twelve tunings.
struct TuningShape
{
  const char* type;
  int instrument;
  int stringCount;
  int intervals[8];
  /// The key it is normally played in, as the note and octave of its lowest string. Every other
  /// key is this one moved down, never up: tuning down is what people do to a guitar, and a
  /// standard tuning transposed up to A would be a set of strings nobody owns.
  int rootNote;
  int rootOctave;
};

/// How a key is spelled where it names a tuning: flats where the convention is flat - Eb standard
/// is not D# standard - and sharps where it is sharp.
const char* const kKeyNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};

// C=0 C#=1 D=2 D#=3 E=4 F=5 F#=6 G=7 G#=8 A=9 A#=10 B=11
constexpr TuningShape kShapes[] = {
  // --- guitar ---
  {"Standard", 0, 6, {0, 5, 10, 15, 19, 24}, 4, 2},
  {"Drop", 0, 6, {0, 7, 12, 17, 21, 26}, 2, 2},
  // The major voicing - root, fifth, octave, third, fifth, octave. In D that is open D, in E open
  // E, in G open G; one shape covers the whole family.
  {"Open", 0, 6, {0, 7, 12, 16, 19, 24}, 2, 2},
  {"DADGAD", 0, 6, {0, 7, 12, 17, 19, 24}, 2, 2},

  {"Standard", 0, 7, {0, 5, 10, 15, 20, 24, 29}, 11, 1},
  {"Drop", 0, 7, {0, 7, 12, 17, 22, 26, 31}, 9, 1},
  {"Open", 0, 7, {0, 7, 12, 19, 24, 28, 31}, 11, 1},

  {"Standard", 0, 8, {0, 5, 10, 15, 20, 25, 29, 34}, 6, 1},
  {"Drop", 0, 8, {0, 7, 12, 17, 22, 27, 31, 36}, 4, 1},

  // --- bass ---
  {"Standard", 1, 4, {0, 5, 10, 15}, 4, 1},
  {"Drop", 1, 4, {0, 7, 12, 17}, 2, 1},
  // Five strings come two ways: one added below the E, or one added above the G. Same intervals,
  // different string to start from, so they are two families rather than two keys.
  {"Standard", 1, 5, {0, 5, 10, 15, 20}, 11, 0},
  {"High C", 1, 5, {0, 5, 10, 15, 20}, 4, 1},
  {"Drop", 1, 5, {0, 7, 12, 17, 22}, 9, 0},
  {"Standard", 1, 6, {0, 5, 10, 15, 20, 25}, 11, 0},
};

/// Every tuning the shapes describe, expanded once: each shape in all twelve keys. Written out by
/// hand this would be several hundred lines of note numbers, and every one a chance to mistype a
/// string that then quietly tunes somebody's guitar wrong.
const std::vector<TuningPreset>& Tunings()
{
  static const std::vector<TuningPreset> tunings = []
  {
    std::vector<TuningPreset> built;
    for (const auto& shape : kShapes)
    {
      const int canonical = (shape.rootOctave + 1) * 12 + shape.rootNote;

      // The home key first, then every semitone below it. That is the order the row is read in -
      // standard, down a half step, down a whole step - and it puts the one most people want at
      // the left rather than in the middle of an alphabetical run.
      for (int step = 0; step < 12; step++)
      {
        const int root = canonical - step;
        const int pitchClass = ((root % 12) + 12) % 12;

        TuningPreset preset{};
        preset.type = shape.type;
        preset.key = kKeyNames[pitchClass];
        preset.instrument = shape.instrument;
        preset.stringCount = shape.stringCount;

        for (int i = 0; i < shape.stringCount; i++)
        {
          const int midi = root + shape.intervals[i];
          preset.strings[i] = TunedString{((midi % 12) + 12) % 12, midi / 12 - 1};
        }
        built.push_back(preset);
      }
    }
    return built;
  }();
  return tunings;
}

/// The tuning a stored instrument, string count and name pick out, or nullptr when they name none.
const TuningPreset* FindTuning(int instrument, int stringCount, const std::string& name)
{
  for (const auto& preset : Tunings())
    if (preset.instrument == instrument && preset.stringCount == stringCount && name == preset.Name())
      return &preset;
  return nullptr;
}

/// The first tuning that exists for an instrument and string count - what to fall back to when a
/// stored name names nothing, which is what an older config's names now do.
const TuningPreset* FirstTuning(int instrument, int stringCount)
{
  for (const auto& preset : Tunings())
    if (preset.instrument == instrument && preset.stringCount == stringCount)
      return &preset;
  return nullptr;
}

/// The strings actually being tuned to: the tuning's own, or the notes put over the top of them by
/// holding a string down and picking another. Overrides are MIDI numbers, one per string.
/// \return how many strings were written
int EffectiveStrings(const TuningPreset& preset, bool custom, const int* overrides, TunedString out[8])
{
  for (int i = 0; i < preset.stringCount; i++)
  {
    if (custom)
    {
      const int midi = overrides[i];
      out[i] = TunedString{((midi % 12) + 12) % 12, midi / 12 - 1};
    }
    else
    {
      out[i] = preset.strings[i];
    }
  }
  return preset.stringCount;
}

/// Cuts a name down to what fits under a block on the chain line. Capture names run long by
/// convention - "Mesa Dual Rectifier Orange Channel Gain 7" - and the line has room for none of it.
std::string Shorten(const std::string& text, size_t limit)
{
  if (text.size() <= limit)
    return text;
  return text.substr(0, limit) + "...";
}

/// Lexicographic compare that ignores case, so a sorted list does not split into an upper-case run
/// followed by a lower-case one the way a raw byte compare would leave it.
/// \return negative, zero or positive, like strcmp
int CompareNoCase(const std::string& a, const std::string& b)
{
  const size_t shared = std::min(a.size(), b.size());
  for (size_t i = 0; i < shared; i++)
  {
    const int left = std::tolower(static_cast<unsigned char>(a[i]));
    const int right = std::tolower(static_cast<unsigned char>(b[i]));
    if (left != right)
      return left - right;
  }
  if (a.size() == b.size())
    return 0;
  return a.size() < b.size() ? -1 : 1;
}

bool FilterCheckbox(const char* label, std::vector<std::string>& target, const std::string& value)
{
  bool selected = VectorContains(target, value);
  if (!theme::Check(label, &selected))
    return false;

  if (selected)
    target.push_back(value);
  else
    target.erase(std::remove(target.begin(), target.end(), value), target.end());
  return true;
}

/// Current selections as removable rows, so a choice stays visible and removable even when the
/// search box below has filtered it out of the list.
bool DrawSelectedFilters(std::vector<std::string>& target)
{
  bool changed = false;
  for (size_t i = 0; i < target.size();)
  {
    ImGui::PushID(static_cast<int>(i));
    const bool remove = ImGui::SmallButton("x");
    ImGui::PopID();
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(target[i].c_str());

    if (remove)
    {
      target.erase(target.begin() + static_cast<std::ptrdiff_t>(i));
      changed = true;
    }
    else
    {
      i++;
    }
  }
  return changed;
}

bool DrawTaxonomyList(const char* id, const std::vector<TaxonomyItem>& items, std::vector<std::string>& target,
                      bool verifiedOnly)
{
  bool changed = false;
  ImGui::BeginChild(id, ImVec2(0, 130), true);
  if (items.empty())
    ImGui::TextDisabled("Loading...");
  for (const auto& item : items)
  {
    if (verifiedOnly && !item.isVerified)
      continue;

    std::string label = item.label;
    if (item.tonesCount > 0)
      label += " (" + std::to_string(item.tonesCount) + ")";

    ImGui::PushID(item.name.c_str());
    if (FilterCheckbox(label.c_str(), target, item.name))
      changed = true;
    ImGui::PopID();
  }
  ImGui::EndChild();
  return changed;
}

enum class NavIcon
{
  Rig,
  Player,
  Tuner,
  Library,
  Metronome,
  Settings
};

/// \brief One destination in the rail: a line glyph, drawn to a common weight and a common box.
///
/// All six are strokes of the same thickness on the same grid, so they read as one set. The
/// previous lot were each drawn to their own scale - an amp that came out looking like a washing
/// machine, a library that was three lines and so read as a menu, a settings glyph that read as a
/// sun - and a set of icons that do not match is worse than no icons at all, because each one has
/// to be learned separately.
/// \brief What a rail icon is being told about the thing it stands for.
///
/// Every field is a reading of something that is happening right now, and every one of them is at
/// rest by default. An icon that moves when nothing is going on is decoration, and decoration in a
/// navigation rail is noise you cannot switch off.
struct NavState
{
  /// Where the metronome's arm is, -1 to 1, with 1 the lean it has when it is still - and how far
  /// the player's mouth is open, 0 to 1. One field because no glyph is ever both.
  float swing = 1.0f;
  /// 0 to 1: the rig's cone being driven past what it can take, or the tuner's tines ringing.
  float shake = 0.0f;
  /// Notes drifting up out of the play icon while something is playing.
  bool playing = false;
  /// A dot in the corner: something finished, or went wrong, while you were looking elsewhere.
  bool badge = false;
  ImVec4 badgeColour = ImVec4(1, 1, 1, 1);
};

void DrawNavGlyph(ImDrawList* draw, NavIcon icon, ImVec2 c, float size, ImU32 colour, const NavState& state)
{
  const float swing = state.swing;
  const float shake = state.shake;

  // A small bob and sway while the player is singing. Two frequencies that do not line up with the
  // mouth's, so the three together read as something moving rather than as one oscillator driving
  // everything at once.
  if (state.playing)
  {
    const float t = static_cast<float>(ImGui::GetTime());
    c.x += std::sin(t * 3.1f) * size * 0.030f;
    c.y += std::sin(t * 5.5f + 0.9f) * size * 0.035f;
  }

  const float r = size * 0.5f;
  const float w = std::max(1.7f, size * 0.085f);
  const auto line = [&](float x0, float y0, float x1, float y1)
  { draw->AddLine(ImVec2(c.x + r * x0, c.y + r * y0), ImVec2(c.x + r * x1, c.y + r * y1), colour, w); };

  switch (icon)
  {
    case NavIcon::Rig:
    {
      // A cab: a box with a speaker in it, and nothing else. A head stacked on a cab with three
      // knobs across it was four shapes competing inside twenty pixels, and none of them read.
      draw->AddRect(ImVec2(c.x - r * 0.92f, c.y - r * 0.92f), ImVec2(c.x + r * 0.92f, c.y + r * 0.92f), colour, 3.0f,
                    0, w);

      // The cone moves and the box does not, which is what a speaker being driven too hard looks
      // like. Two frequencies that do not divide into each other, so it reads as a rattle rather
      // than as something orbiting.
      float ox = 0.0f;
      float oy = 0.0f;
      if (shake > 0.0f)
      {
        const float t = static_cast<float>(ImGui::GetTime());
        ox = std::sin(t * 47.0f) * r * 0.11f * shake;
        oy = std::sin(t * 61.0f + 1.7f) * r * 0.11f * shake;
      }

      // Red only while it is clipping. This is the one thing in the rail worth a colour: it is the
      // difference between a rig that sounds like that on purpose and one that is breaking up.
      const ImU32 cone = (shake > 0.0f) ? ImGui::GetColorU32(theme::Danger()) : colour;
      draw->AddCircle(ImVec2(c.x + ox, c.y + oy), r * 0.46f, cone, 24, w);
      break;
    }

    case NavIcon::Player:
    {
      // The play triangle, split along the line from the middle of its back edge to its point, so
      // the two halves can open at the tip like a beak. The back edge stays put and only the point
      // parts, which is what makes it read as a mouth rather than as the whole shape hinging.
      //
      // Shut, the two halves share that line exactly and the result is the plain triangle it has
      // always been - so an icon that is not playing has nothing about it to explain.
      const float gap = r * 0.34f * std::clamp(swing, 0.0f, 1.0f); // swing is the mouth here
      const ImVec2 back(c.x - r * 0.58f, c.y);
      const ImVec2 top(c.x - r * 0.58f, c.y - r * 0.86f);
      const ImVec2 bottom(c.x - r * 0.58f, c.y + r * 0.86f);
      const float tipX = c.x + r * 0.86f;

      draw->AddTriangleFilled(top, back, ImVec2(tipX, c.y - gap), colour);
      draw->AddTriangleFilled(back, bottom, ImVec2(tipX, c.y + gap), colour);

      // An eye, but only while it is singing - a triangle standing still has no business having
      // one. Punched in the rail's own colour so it is a hole in the shape rather than a dot laid
      // on top of it.
      if (state.playing)
        draw->AddCircleFilled(ImVec2(c.x - r * 0.18f, c.y - r * 0.36f), std::max(1.5f, r * 0.14f),
                              ImGui::GetColorU32(theme::Rail()), 10);
      break;
    }

    case NavIcon::Tuner:
    {
      // A tuning fork: two tines, a shoulder and a stem.
      //
      // The tines flex apart and together while a string is ringing, held at the shoulder the way
      // a real fork is - so the whole glyph does not slide about, only the part that would move.
      // The amount follows how loud the string still is, which means it dies away by itself as the
      // note does rather than needing to be told when to stop.
      const float flex = (shake > 0.0f) ? std::sin(static_cast<float>(ImGui::GetTime()) * 38.0f) * 0.16f * shake : 0.0f;

      line(-0.46f - flex, -0.95f, -0.46f, 0.05f);
      line(0.46f + flex, -0.95f, 0.46f, 0.05f);
      draw->PathLineTo(ImVec2(c.x - r * 0.46f, c.y + r * 0.05f));
      draw->PathBezierQuadraticCurveTo(ImVec2(c.x, c.y + r * 0.42f), ImVec2(c.x + r * 0.46f, c.y + r * 0.05f));
      draw->PathStroke(colour, 0, w);
      line(0.0f, 0.30f, 0.0f, 0.98f);
      break;
    }

    case NavIcon::Metronome:
    {
      // The case, and the arm swinging inside it.
      draw->PathLineTo(ImVec2(c.x - r * 0.78f, c.y + r * 0.92f));
      draw->PathLineTo(ImVec2(c.x - r * 0.24f, c.y - r * 0.92f));
      draw->PathLineTo(ImVec2(c.x + r * 0.24f, c.y - r * 0.92f));
      draw->PathLineTo(ImVec2(c.x + r * 0.78f, c.y + r * 0.92f));
      draw->PathStroke(colour, ImDrawFlags_Closed, w);
      line(-0.60f, 0.44f, 0.60f, 0.44f);

      // Pivoted where the arm meets the base, so it sweeps rather than slides. The lean it has at
      // rest is one end of the travel, which is why swing runs from -1 to 1 with 1 being the icon
      // exactly as it was drawn before any of this moved.
      constexpr float kPivotX = -0.16f;
      constexpr float kPivotY = 0.40f;
      constexpr float kLength = 1.12f;
      constexpr float kLean = 0.424f; // 24 degrees, the angle the still icon leans at
      const float angle = std::clamp(swing, -1.0f, 1.0f) * kLean;
      line(kPivotX, kPivotY, kPivotX + std::sin(angle) * kLength, kPivotY - std::cos(angle) * kLength);
      break;
    }

    case NavIcon::Library:
      // Books stood on a shelf, at different heights. Horizontal lines read as a menu.
      draw->AddRect(ImVec2(c.x - r * 0.92f, c.y - r * 0.66f), ImVec2(c.x - r * 0.40f, c.y + r * 0.86f), colour, 1.5f,
                    0, w);
      draw->AddRect(ImVec2(c.x - r * 0.26f, c.y - r * 0.90f), ImVec2(c.x + r * 0.26f, c.y + r * 0.86f), colour, 1.5f,
                    0, w);
      draw->AddRect(ImVec2(c.x + r * 0.40f, c.y - r * 0.44f), ImVec2(c.x + r * 0.92f, c.y + r * 0.86f), colour, 1.5f,
                    0, w);
      break;

    case NavIcon::Settings:
      // A gear: a ring with eight teeth. The old one was a circle inside a dashed circle, which at
      // this size is a sun.
      draw->AddCircle(c, r * 0.44f, colour, 24, w);
      for (int i = 0; i < 8; i++)
      {
        const float a = static_cast<float>(i) * 0.7853981f;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        draw->AddLine(ImVec2(c.x + ca * r * 0.62f, c.y + sa * r * 0.62f),
                      ImVec2(c.x + ca * r * 0.95f, c.y + sa * r * 0.95f), colour, w);
      }
      break;
  }
}

/// \brief One entry in the vertical rail.
///
/// The mark for "you are here" is a bar against the rail's edge rather than a pill behind the
/// glyph: in a column, the edge is the thing all six share, so a mark on it reads as a position in
/// a list rather than as a sixth button that happens to be lit.
/// \brief One note drifting up out of the play icon.
///
/// File scope rather than a member, the same way the scrollbar's grab state is: this is entirely a
/// property of the drawing, there is exactly one play icon on the screen, and threading it through
/// the window class would put six floats of animation into the header for no one to use.
struct NoteBubble
{
  float rise = 0.0f;  ///< 0 where it appears, 1 at the top where it pops
  float speed = 1.0f; ///< so they do not climb as a block
  float sway = 0.0f;  ///< where its sideways wander starts in the cycle
  float scale = 1.0f; ///< so a run of them is not one size repeated either
  float pop = 0.0f;   ///< counts up once it has burst, and fades the ring out
  bool tail = false;  ///< the two note shapes, so a run of them is not one shape repeated
  bool alive = false;
};

std::array<NoteBubble, 5> gNoteBubbles{};
double gNextNoteAt = 0.0;
/// Enough of a sequence to look unplanned, without the weight of a random engine.
unsigned int gNoteSeed = 1u;

float NextNoteRandom()
{
  gNoteSeed = gNoteSeed * 1664525u + 1013904223u;
  return static_cast<float>((gNoteSeed >> 8) & 0xFFFF) / 65535.0f;
}

/// Draws and advances the notes. `spawning` is false while they are being left to finish after
/// the music has stopped, so the last few still rise and pop rather than vanishing mid-air.
/// \param tint the icon's own colour: the notes come out of it, so they are made of the same thing
void DrawNoteBubbles(ImDrawList* draw, ImVec2 centre, float size, bool spawning, ImVec4 tint)
{
  constexpr float kPi = 3.14159265358979323846f;
  const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f); // a stalled frame must not teleport them
  const double now = ImGui::GetTime();

  // Out of the right-hand edge of the triangle, which is the point it aims at - so they look like
  // they are coming out of the icon rather than off it.
  const ImVec2 from(centre.x + size * 0.30f, centre.y + size * 0.10f);
  const float travel = size * 0.86f;

  if (spawning && now >= gNextNoteAt)
  {
    for (auto& note : gNoteBubbles)
    {
      if (note.alive)
        continue;
      note = NoteBubble{};
      note.alive = true;
      note.speed = 0.85f + NextNoteRandom() * 0.5f;
      note.sway = NextNoteRandom() * kPi * 2.0f;
      note.scale = 0.72f + NextNoteRandom() * 0.66f;
      note.tail = NextNoteRandom() > 0.5f;
      break;
    }
    // Sparse enough that each one is its own event. A steady stream reads as a texture, and a
    // texture in the corner of a navigation rail is something you end up wanting to switch off.
    gNextNoteAt = now + 0.62 + static_cast<double>(NextNoteRandom()) * 0.50;
  }

  for (auto& note : gNoteBubbles)
  {
    if (!note.alive)
      continue;

    if (note.pop > 0.0f)
    {
      // Burst: a thin ring opening out and fading. Short, because it is punctuation and not an
      // event of its own.
      note.pop += dt * 5.0f;
      if (note.pop >= 1.0f)
      {
        note.alive = false;
        continue;
      }
      const float radius = size * (0.045f + 0.08f * note.pop) * note.scale;
      draw->AddCircle(ImVec2(from.x + std::sin(note.sway) * size * 0.10f, from.y - travel), radius,
                      ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.55f * (1.0f - note.pop))), 14, 1.3f);
      continue;
    }

    note.rise += dt * 0.55f * note.speed;
    if (note.rise >= 1.0f)
    {
      note.pop = 0.001f;
      continue;
    }

    // A slow wander sideways, so five of them do not climb in a column.
    const float x = from.x + std::sin(note.sway + note.rise * 3.4f) * size * 0.10f;
    const float y = from.y - travel * note.rise;

    // Fading as it climbs: the top of the cell is where it goes, and something that vanishes at
    // full strength reads as a glitch rather than as distance.
    const float alpha = 0.85f * (1.0f - note.rise * note.rise);
    const ImU32 colour = ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, alpha));

    // A note at four pixels: a filled head, a stem, and a flag on half of them. Anything more
    // detailed is a smudge at this size. Each gets its own size, so the run reads as a handful of
    // notes rather than as one note repeated.
    const float head = size * 0.058f * note.scale;
    draw->AddCircleFilled(ImVec2(x, y), head, colour, 10);
    draw->AddLine(ImVec2(x + head * 0.85f, y), ImVec2(x + head * 0.85f, y - head * 3.0f), colour, 1.2f);
    if (note.tail)
      draw->AddLine(ImVec2(x + head * 0.85f, y - head * 3.0f), ImVec2(x + head * 2.2f, y - head * 1.9f), colour, 1.2f);
  }
}

bool NavRailButton(const char* id, NavIcon icon, bool active, float railWidth, float size,
                   const NavState& state = NavState())
{
  ImGui::PushID(id);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("nav", ImVec2(railWidth, size));
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 centre(pos.x + railWidth * 0.5f, pos.y + size * 0.5f);

  if (hovered && !active)
    draw->AddRectFilled(ImVec2(pos.x + 6.0f, pos.y + 3.0f), ImVec2(pos.x + railWidth - 6.0f, pos.y + size - 3.0f),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f)), 8.0f);

  if (active)
    draw->AddRectFilled(ImVec2(pos.x, centre.y - size * 0.30f), ImVec2(pos.x + 2.5f, centre.y + size * 0.30f),
                        ImGui::GetColorU32(theme::Accent()), 1.5f);

  const ImU32 colour = ImGui::GetColorU32(active ? theme::Control() : (hovered ? theme::Control() : theme::TextDim()));
  DrawNavGlyph(draw, icon, centre, size * 0.52f, colour, state);

  // Only the play icon has them, and they go on rising after the music stops so the last few
  // finish their climb rather than blinking out in mid-air.
  if (icon == NavIcon::Player)
  {
    bool anyLeft = false;
    for (const auto& note : gNoteBubbles)
      anyLeft = anyLeft || note.alive;
    if (state.playing || anyLeft)
      DrawNoteBubbles(draw, centre, size, state.playing,
                      active || hovered ? theme::Control() : theme::TextDim());
  }

  // --- a dot in the corner, for something that happened while you were elsewhere ---
  if (state.badge)
  {
    const ImVec2 at(centre.x + size * 0.30f, centre.y - size * 0.30f);
    // Rimmed in the rail's own colour, so it stays a dot rather than merging into the glyph
    // whenever the two happen to overlap.
    draw->AddCircleFilled(at, 4.6f, ImGui::GetColorU32(theme::Rail()), 12);
    draw->AddCircleFilled(at, 3.2f, ImGui::GetColorU32(state.badgeColour), 12);
  }

  return clicked;
}

/// Navigation drawn as glyphs rather than words: destinations don't need labels once you know
/// them, and the rail stays quiet.

/// \brief One row of a block's settings: the name, a slider, and the value.
///
/// The shape every control in a block has, so a block reads as one list of the same thing rather
/// than as knobs here, a slider there and a drag field somewhere else. Stacked, the names line up
/// down the left and the values down the right, which is what makes eight of them scannable.
///
/// \param labelWidth the column the names line up in
/// \param valueText overrides the printed value, for controls whose number is not the raw one
/// \return true while it is being moved
bool BlockControl(const char* id, const char* name, float* value, float minValue, float maxValue, float defaultValue,
                  const char* format, float labelWidth = 96.0f, const char* valueText = nullptr)
{
  (void)labelWidth; // kept so callers can go on naming their column

  ImGui::PushID(id);

  // Name at the left, value at the right, and the slider across the whole width underneath. Name,
  // slider and value on one line left the slider a stub in the middle of the row; the point of a
  // horizontal control is the travel, and the travel should be the width of the panel.
  theme::Label(name);

  char printed[24];
  if (valueText == nullptr)
  {
    std::snprintf(printed, sizeof(printed), format, *value);
    valueText = printed;
  }

  ImGui::SameLine();
  const float valueWidth = ImGui::CalcTextSize(valueText).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - valueWidth);
  ImGui::TextUnformatted(valueText);

  const bool changed =
    theme::SlimSlider("slider", value, minValue, maxValue, defaultValue, ImGui::GetContentRegionAvail().x, format);
  ImGui::Dummy(ImVec2(0.0f, 2.0f));

  ImGui::PopID();
  return changed;
}

/// \brief A vertical fader with its name and value under it.
///
/// The one place a block uses these is beside the EQ's curve, where three of them stood on end fit
/// in the height the plot already has - and where the horizontal rows they replace would have made
/// the panel scroll to reach the third one.
///
/// \param width the column the whole control occupies; the fader is centred in it
/// \return true while it is being moved
bool VerticalControl(const char* id, const char* name, float* value, float minValue, float maxValue,
                     float defaultValue, const char* format, float height, float width = 70.0f)
{
  constexpr float kFaderWidth = 16.0f;

  ImGui::PushID(id);
  ImGui::BeginGroup();

  const float left = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(left + (width - kFaderWidth) * 0.5f);
  const bool changed = theme::SlimSliderVertical("fader", value, minValue, maxValue, defaultValue, height, format);
  const bool hovered = ImGui::IsItemHovered();

  char printed[24];
  std::snprintf(printed, sizeof(printed), format, *value);

  const auto centred = [&](const char* text, ImVec4 colour)
  {
    ImGui::SetCursorPosX(left + (width - ImGui::CalcTextSize(text).x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
  };

  ImGui::Spacing();
  centred(name, theme::TextDim());
  centred(printed, theme::Text());

  ImGui::EndGroup();
  ImGui::PopID();

  if (hovered)
    ImGui::SetTooltip("%s", name);
  return changed;
}

/// A segmented control: one row of buttons where exactly one is active.
bool SegmentedButton(const char* label, bool active, float width)
{
  if (active)
  {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::AccentDim());
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  }
  const bool clicked = ImGui::Button(label, ImVec2(width, 0));
  if (active)
    ImGui::PopStyleColor(2);
  return clicked;
}

} // namespace

MainWindow::MainWindow(GLFWwindow* window)
: mWindow(window)
{
  mConfig = AppConfig::Load();
  mAutoNormalize = mConfig.autoNormalize;
  mBufferFramesValue = static_cast<int>(mConfig.bufferFrames > 0 ? mConfig.bufferFrames : 256);

  // The app's own key unless this machine has been told to use a different one. A user who never
  // opens Advanced never sees either.
  mTone3000.SetClientId(mConfig.tone3000ClientId.empty() ? std::string(kTone3000AppClientId)
                                                         : mConfig.tone3000ClientId);
  std::snprintf(mClientIdInput, sizeof(mClientIdInput), "%s", mConfig.tone3000ClientId.c_str());
  if (!mConfig.tone3000RefreshTokenEncrypted.empty())
  {
    mPersistedRefreshToken = DecryptSecret(mConfig.tone3000RefreshTokenEncrypted);
    mTone3000.SetRefreshToken(mPersistedRefreshToken);
  }

  for (size_t i = 0; i < std::size(kCommonSampleRates); i++)
    if (kCommonSampleRates[i] == mConfig.sampleRate)
      mSelectedSampleRateIndex = static_cast<int>(i);

  RtAudio::getCompiledApi(mCompiledApis);
  if (mCompiledApis.empty())
    mCompiledApis.push_back(RtAudio::UNSPECIFIED);

  if (!mConfig.apiName.empty())
  {
    const RtAudio::Api wanted = RtAudio::getCompiledApiByName(mConfig.apiName);
    for (size_t i = 0; i < mCompiledApis.size(); i++)
      if (mCompiledApis[i] == wanted)
        mSelectedApiIndex = static_cast<int>(i);
  }

  RefreshDeviceLists();

  if (!mConfig.captureFolder.empty())
    mCaptureLibrary.SetFolder(mConfig.captureFolder);
  mCaptureLibrary.SetUserTags(mConfig.userTags);

  mSelectedInputChannel = std::max(0, mConfig.inputChannel);
  mSelectedOutputChannel = std::max(0, mConfig.outputChannel);

  if (mSelectedInputIndex >= 0 && mSelectedOutputIndex >= 0)
    ApplyDeviceSelection();

  mEngine.SetInputGainDb(mConfig.inputGainDb);
  mEngine.SetOutputGainDb(mConfig.outputGainDb);
  mEngine.SetBypassed(mConfig.bypassed);

  GateSettings gate;
  gate.enabled = mConfig.gateEnabled;
  gate.placement = static_cast<GatePlacement>(mConfig.gatePlacement == 1 ? 1 : 0);
  gate.thresholdDb = mConfig.gateThresholdDb;
  mEngine.SetGate(gate);

  mTunerNoteIndex = std::clamp(mConfig.tunerNoteIndex, 0, 11);
  mTunerOctave = std::clamp(mConfig.tunerOctave, 0, 8);
  mTunerA4Hz = std::clamp(mConfig.tunerA4Hz, 415.0f, 466.0f);
  mTunerMutesOutput = mConfig.tunerMutesOutput;
  mTunerNeedleMode = mConfig.tunerNeedleMode;
  mTunerAuto = mConfig.tunerAuto;
  mTunerCustomStrings = !mConfig.tunerCustomStrings.empty();
  for (size_t i = 0; i < mConfig.tunerCustomStrings.size() && i < mTunerStringMidi.size(); i++)
    mTunerStringMidi[i] = mConfig.tunerCustomStrings[i];
  mMetronome = mConfig.metronome;
  mMetronomeVisual = mConfig.metronomeVisual;
  std::snprintf(mDemucsCommand, sizeof(mDemucsCommand), "%s", mConfig.demucsCommand.c_str());
  std::snprintf(mTranscribeCommand, sizeof(mTranscribeCommand), "%s", mConfig.transcribeCommand.c_str());
  if (mConfig.fullscreen)
    SetFullscreen(true);
  ApplyMetronome();
  mTunerInstrument = mConfig.tunerInstrument;
  mTunerStringCount = mConfig.tunerStringCount;
  mTunerTuningName = mConfig.tunerTuningName;
  UpdateTunerReference();

  RestoreChainFromConfig();

  // Nothing to browse and no device chosen yet, so start where those are set rather than on an
  // empty chain.
  if (mConfig.captureFolder.empty())
    mView = View::Settings;
}

MainWindow::~MainWindow()
{
  mConfig.autoNormalize = mAutoNormalize;
  mConfig.Save();
}

void MainWindow::RefreshDeviceLists()
{
  const RtAudio::Api api = mCompiledApis[static_cast<size_t>(mSelectedApiIndex)];
  mInputDevices.clear();
  mOutputDevices.clear();
  for (const auto& device : mEngine.GetDevices(api))
  {
    if (device.inputChannels > 0)
      mInputDevices.push_back(device);
    if (device.outputChannels > 0)
      mOutputDevices.push_back(device);
  }

  mSelectedInputIndex = -1;
  for (size_t i = 0; i < mInputDevices.size(); i++)
    if (mInputDevices[i].name == mConfig.inputDeviceName
        || (mConfig.inputDeviceName.empty() && mInputDevices[i].isDefaultInput))
      mSelectedInputIndex = static_cast<int>(i);
  if (mSelectedInputIndex < 0 && !mInputDevices.empty())
    mSelectedInputIndex = 0;

  mSelectedOutputIndex = -1;
  for (size_t i = 0; i < mOutputDevices.size(); i++)
    if (mOutputDevices[i].name == mConfig.outputDeviceName
        || (mConfig.outputDeviceName.empty() && mOutputDevices[i].isDefaultOutput))
      mSelectedOutputIndex = static_cast<int>(i);
  if (mSelectedOutputIndex < 0 && !mOutputDevices.empty())
    mSelectedOutputIndex = 0;
}

void MainWindow::ApplyDeviceSelection()
{
  if (mSelectedInputIndex < 0 || mSelectedOutputIndex < 0)
    return;

  const RtAudio::DeviceInfo& inDevice = mInputDevices[static_cast<size_t>(mSelectedInputIndex)];
  const RtAudio::DeviceInfo& outDevice = mOutputDevices[static_cast<size_t>(mSelectedOutputIndex)];

  // A channel remembered from another interface can point at a socket this one does not have.
  mSelectedInputChannel = std::clamp(mSelectedInputChannel, 0, std::max(0, static_cast<int>(inDevice.inputChannels) - 1));
  mSelectedOutputChannel =
    std::clamp(mSelectedOutputChannel, 0, std::max(0, static_cast<int>(outDevice.outputChannels) - 2));

  AudioDeviceChoice choice;
  choice.api = mCompiledApis[static_cast<size_t>(mSelectedApiIndex)];
  choice.inputDeviceId = inDevice.ID;
  choice.outputDeviceId = outDevice.ID;
  choice.inputFirstChannel = static_cast<unsigned int>(mSelectedInputChannel);
  choice.outputFirstChannel = static_cast<unsigned int>(mSelectedOutputChannel);
  choice.sampleRate = kCommonSampleRates[static_cast<size_t>(mSelectedSampleRateIndex)];
  choice.bufferFrames = static_cast<unsigned int>(std::max(mBufferFramesValue, 16));

  if (!mEngine.Open(choice).empty())
    return;

  mConfig.apiName = RtAudio::getApiName(choice.api);
  mConfig.inputDeviceName = inDevice.name;
  mConfig.outputDeviceName = outDevice.name;
  mConfig.inputChannel = mSelectedInputChannel;
  mConfig.outputChannel = mSelectedOutputChannel;
  mConfig.sampleRate = mEngine.GetActualSampleRate();
  mConfig.bufferFrames = mEngine.GetActualBufferFrames();
  mConfig.Save();

  // The stream settings changed, so everything loaded was prepared for the old ones. IRs in
  // particular were resampled to the previous rate and have to be rebuilt.
  ReloadChain();
}

int MainWindow::AddBlock(BlockType type)
{
  const int id = mEngine.AddBlock(type);
  if (id > 0)
  {
    mSelectedBlockId = id;
    SaveChainToConfig();
  }
  return id;
}

void MainWindow::RemoveBlock(int blockId)
{
  mCaptureLibrary.CancelLoads(blockId);
  mEngine.RemoveBlock(blockId);
  mBlockPaths.erase(blockId);
  mBlockErrors.erase(blockId);
  if (mSelectedBlockId == blockId)
    mSelectedBlockId = 0;
  mLastMatchedCapture.clear();
  SaveChainToConfig();
}

void MainWindow::LoadFileIntoBlock(int blockId, const std::filesystem::path& path)
{
  BlockSettings settings;
  if (!mEngine.GetBlockSettings(blockId, settings))
    return;

  const std::string ext = ToLower(path.extension().string());
  const BlockType fileType = (ext == ".wav") ? BlockType::Ir : BlockType::Nam;

  // Dropping a WAV on a NAM block (or the reverse) retypes the block rather than refusing: the
  // block is just a position in the chain, and this is what the user plainly meant.
  if (settings.type != fileType)
  {
    settings.type = fileType;
    mEngine.SetBlockSettings(blockId, settings);
  }

  const double sampleRate =
    mEngine.IsOpen() ? static_cast<double>(mEngine.GetActualSampleRate()) : static_cast<double>(mConfig.sampleRate);
  const int maxBuffer = static_cast<int>(
    std::max(mEngine.IsOpen() ? mEngine.GetActualBufferFrames() : mConfig.bufferFrames, kMaxBlockFrames));

  mCaptureLibrary.RequestLoad(blockId, fileType, path, sampleRate, maxBuffer);
  mBlockPaths[blockId] = path;
  mBlockErrors.erase(blockId);

  mConfig.NoteRecent(path.string());
  SaveChainToConfig();
}

void MainWindow::LoadLibraryEntry(const CaptureEntry& entry)
{
  const BlockType wanted = entry.isIr ? BlockType::Ir : BlockType::Nam;

  // Prefer the selected block, then any block of the right type, and otherwise make one. That
  // way clicking a file always does something sensible however the chain happens to be set up.
  int target = 0;
  BlockSettings settings;
  if (mSelectedBlockId != 0 && mEngine.GetBlockSettings(mSelectedBlockId, settings))
    target = mSelectedBlockId;

  if (target == 0)
  {
    for (const auto& block : mEngine.GetBlocks())
    {
      if (block.settings.type == wanted)
      {
        target = block.id;
        break;
      }
    }
  }

  if (target == 0)
    target = AddBlock(wanted);
  if (target > 0)
    LoadFileIntoBlock(target, entry.path);
}

void MainWindow::PumpLoads()
{
  LoadResult result;
  while (mCaptureLibrary.PopCompleted(result))
  {
    if (!result.error.empty())
    {
      mBlockErrors[result.blockId] = result.error;
      continue;
    }

    mBlockErrors.erase(result.blockId);
    if (result.type == BlockType::Nam)
      mEngine.SetBlockNamModel(result.blockId, std::move(result.namModel));
    else
      mEngine.SetBlockIr(result.blockId, std::move(result.ir));

    mLastMatchedCapture.clear();
  }

  ApplyLoudnessMatch();
}

void MainWindow::ReloadChain()
{
  const double sampleRate =
    mEngine.IsOpen() ? static_cast<double>(mEngine.GetActualSampleRate()) : static_cast<double>(mConfig.sampleRate);
  const int maxBuffer = static_cast<int>(
    std::max(mEngine.IsOpen() ? mEngine.GetActualBufferFrames() : mConfig.bufferFrames, kMaxBlockFrames));

  for (const auto& block : mEngine.GetBlocks())
  {
    const auto found = mBlockPaths.find(block.id);
    if (found == mBlockPaths.end() || found->second.empty())
      continue;
    mCaptureLibrary.RequestLoad(block.id, block.settings.type, found->second, sampleRate, maxBuffer);
  }
}

void MainWindow::SaveChainToConfig()
{
  mConfig.blocks.clear();
  for (const auto& block : mEngine.GetBlocks())
  {
    AppConfig::BlockConfig stored;
    stored.type = static_cast<int>(block.settings.type);
    stored.enabled = block.settings.enabled;
    stored.gainDb = block.settings.gainDb;
    stored.levelDb = block.settings.levelDb;
    stored.dryBlend = block.settings.dryBlend;
    stored.lowCutHz = block.settings.lowCutHz;
    stored.highCutHz = block.settings.highCutHz;
    stored.compPeak = block.settings.compPeak;
    stored.compLimit = block.settings.compLimit;
    for (const auto& band : block.settings.peq.bands)
      stored.peqBands.push_back({band.enabled, band.hz, band.gainDb, band.q});
    stored.row = block.settings.row;
    stored.eq.enabled = block.settings.eq.enabled;
    stored.eq.placement = static_cast<int>(block.settings.eq.placement);
    stored.eq.lowDb = block.settings.eq.lowDb;
    stored.eq.midDb = block.settings.eq.midDb;
    stored.eq.highDb = block.settings.eq.highDb;
    stored.eq.midHz = block.settings.eq.midHz;

    const auto found = mBlockPaths.find(block.id);
    if (found != mBlockPaths.end())
      stored.path = found->second.string();

    mConfig.blocks.push_back(std::move(stored));
  }
  mConfig.Save();
}

void MainWindow::ApplyChain(const std::vector<AppConfig::BlockConfig>& blocks,
                            const std::vector<AppConfig::SectionConfig>& sections)
{
  // Everything currently in the chain goes first, so this reads as "the chain is now that one"
  // rather than "that one is appended to whatever was here".
  for (const auto& existing : mEngine.GetBlocks())
  {
    mCaptureLibrary.CancelLoads(existing.id);
    mEngine.RemoveBlock(existing.id);
  }
  mBlockPaths.clear();
  mBlockErrors.clear();

  std::error_code ec;
  for (const auto& stored : blocks)
  {
    const BlockType type = (stored.type == 1)   ? BlockType::Ir
                           : (stored.type == 2) ? BlockType::Cut
                           : (stored.type == 3) ? BlockType::Comp
                           : (stored.type == 4) ? BlockType::Eq
                                                : BlockType::Nam;
    const int id = mEngine.AddBlock(type);
    if (id < 0)
      break; // chain is full; the rest of the saved blocks are dropped

    BlockSettings settings;
    settings.type = type;
    settings.enabled = stored.enabled;
    settings.gainDb = stored.gainDb;
    settings.levelDb = stored.levelDb;
    settings.dryBlend = stored.dryBlend;
    settings.lowCutHz = stored.lowCutHz;
    settings.highCutHz = stored.highCutHz;
    settings.compPeak = stored.compPeak;
    settings.compLimit = stored.compLimit;
    for (size_t b = 0; b < kEqBandCount && b < stored.peqBands.size(); b++)
    {
      settings.peq.bands[b].enabled = stored.peqBands[b].enabled;
      settings.peq.bands[b].hz = stored.peqBands[b].hz;
      settings.peq.bands[b].gainDb = stored.peqBands[b].gainDb;
      settings.peq.bands[b].q = stored.peqBands[b].q;
    }
    settings.row = (stored.row == 1) ? 1 : 0;
    settings.eq.enabled = stored.eq.enabled;
    settings.eq.placement = static_cast<EqPlacement>(stored.eq.placement == 0 ? 0 : 1);
    settings.eq.lowDb = stored.eq.lowDb;
    settings.eq.midDb = stored.eq.midDb;
    settings.eq.highDb = stored.eq.highDb;
    settings.eq.midHz = stored.eq.midHz;
    mEngine.SetBlockSettings(id, settings);

    if (!stored.path.empty() && std::filesystem::exists(stored.path, ec))
    {
      mBlockPaths[id] = stored.path;
      LoadFileIntoBlock(id, stored.path);
    }
  }

  ChainRouting routing;
  for (const auto& stored : sections)
  {
    if (routing.sectionCount >= kMaxParallelSections)
      break;
    ParallelSection& section = routing.sections[routing.sectionCount++];
    section.splitIndex = static_cast<size_t>(std::max(0, stored.splitIndex));
    section.mergeIndex = static_cast<size_t>(std::max(0, stored.mergeIndex));
    section.mode = (stored.mode == 1) ? SplitMode::Crossover : SplitMode::Full;
    section.crossoverHz = stored.crossoverHz;
    section.upperDb = stored.upperDb;
    section.lowerDb = stored.lowerDb;
    section.mergeLevelDb = stored.mergeLevelDb;
  }
  mEngine.SetRouting(routing);

  const auto rebuilt = mEngine.GetBlocks();
  mSelectedBlockId = rebuilt.empty() ? 0 : rebuilt.front().id;
  mLastMatchedCapture.clear(); // loudness matching re-applies to whatever is loaded now
}

void MainWindow::RestoreChainFromConfig()
{
  ApplyChain(mConfig.blocks, mConfig.sections);

  // The rig it was last on comes back with it, by name - so the app opens on what you were
  // playing rather than on an "Unsaved rig" that happens to hold the same blocks. The chain has
  // already been restored above, so a rig file that has since been deleted costs nothing: the
  // name is simply dropped.
  if (!mConfig.currentRigName.empty())
  {
    RigFile rig;
    if (RigFile::Load(mConfig.currentRigName, rig))
      mCurrentRigName = mConfig.currentRigName;
    else
      mConfig.currentRigName.clear();
  }

  // A first run has no saved chain; a capture and a cab is the shape most rigs start from.
  if (mConfig.blocks.empty())
  {
    mEngine.AddBlock(BlockType::Nam);
    mEngine.AddBlock(BlockType::Ir);
    const auto blocks = mEngine.GetBlocks();
    mSelectedBlockId = blocks.empty() ? 0 : blocks.front().id;
  }
}

void MainWindow::SaveRig(const std::string& name)
{
  if (name.empty())
    return;

  RigFile rig;
  rig.name = name;

  for (const auto& block : mEngine.GetBlocks())
  {
    AppConfig::BlockConfig stored;
    stored.type = static_cast<int>(block.settings.type);
    stored.enabled = block.settings.enabled;
    stored.gainDb = block.settings.gainDb;
    stored.levelDb = block.settings.levelDb;
    stored.dryBlend = block.settings.dryBlend;
    stored.lowCutHz = block.settings.lowCutHz;
    stored.highCutHz = block.settings.highCutHz;
    stored.compPeak = block.settings.compPeak;
    stored.compLimit = block.settings.compLimit;
    for (const auto& band : block.settings.peq.bands)
      stored.peqBands.push_back({band.enabled, band.hz, band.gainDb, band.q});
    stored.row = block.settings.row;
    stored.eq.enabled = block.settings.eq.enabled;
    stored.eq.placement = static_cast<int>(block.settings.eq.placement);
    stored.eq.lowDb = block.settings.eq.lowDb;
    stored.eq.midDb = block.settings.eq.midDb;
    stored.eq.highDb = block.settings.eq.highDb;
    stored.eq.midHz = block.settings.eq.midHz;

    const auto found = mBlockPaths.find(block.id);
    if (found != mBlockPaths.end())
      stored.path = found->second.string();

    rig.blocks.push_back(std::move(stored));
  }

  const ChainRouting routing = mEngine.GetRouting();
  for (size_t s = 0; s < routing.sectionCount; s++)
  {
    const ParallelSection& section = routing.sections[s];
    rig.sections.push_back({static_cast<int>(section.splitIndex), static_cast<int>(section.mergeIndex),
                            static_cast<int>(section.mode), section.crossoverHz, section.mix, section.upperDb,
                            section.lowerDb, section.mergeLevelDb});
  }

  const GateSettings gate = mEngine.GetGate();
  rig.gateEnabled = gate.enabled;
  rig.gatePlacement = static_cast<int>(gate.placement);
  rig.gateThresholdDb = gate.thresholdDb;

  if (rig.Save())
  {
    mCurrentRigName = name;
    mRigNamesLoaded = false;
    // Remembered so the app opens on the rig it was closed on, name and all.
    mConfig.currentRigName = name;
    mConfig.Save();
  }
}

void MainWindow::LoadRig(const std::string& name)
{
  RigFile rig;
  if (!RigFile::Load(name, rig))
    return;

  ApplyChain(rig.blocks, rig.sections);

  GateSettings gate;
  gate.enabled = rig.gateEnabled;
  // Always before the chain now, whatever an older rig file says. Gating after the amp gates the
  // amp's own noise back in, so the choice was never one worth offering.
  gate.placement = GatePlacement::Pre;
  gate.thresholdDb = rig.gateThresholdDb;
  mEngine.SetGate(gate);
  mConfig.gateEnabled = gate.enabled;
  mConfig.gatePlacement = 0;
  mConfig.gateThresholdDb = gate.thresholdDb;

  mCurrentRigName = rig.name;

  // The loaded rig is also the session's chain, so it survives a restart without being re-loaded.
  SaveChainToConfig();
  SaveRoutingToConfig();
  mConfig.currentRigName = rig.name;
  mConfig.Save();
}

void MainWindow::ApplyLoudnessMatch()
{
  if (!mAutoNormalize)
    return;

  // Output loudness is set by the last capture that is actually running. Its own loudness figure
  // assumes a standard input, which anything before it has changed - so this is a good starting
  // point rather than an exact match. Said plainly in the checkbox tooltip.
  std::shared_ptr<nam::DSP> last;
  std::filesystem::path lastPath;
  for (const auto& block : mEngine.GetBlocks())
  {
    if (!block.settings.enabled || !block.namModel)
      continue;
    last = block.namModel;
    const auto found = mBlockPaths.find(block.id);
    lastPath = (found != mBlockPaths.end()) ? found->second : std::filesystem::path();
  }

  if (!last || lastPath.empty() || lastPath == mLastMatchedCapture)
    return;

  mLastMatchedCapture = lastPath;

  const float gain = last->HasLoudness() ? kTargetLoudnessDb - static_cast<float>(last->GetLoudness()) : 0.0f;
  mEngine.SetOutputGainDb(std::clamp(gain, -24.0f, 24.0f));
  mConfig.outputGainDb = mEngine.GetOutputGainDb();
}

namespace
{
/// The monitor the window sits on, by how much of it each one covers. Going fullscreen on the
/// primary screen when the window is on the second one is the sort of thing that is only wrong
/// for people with two screens - which is most people at a desk.
GLFWmonitor* MonitorForWindow(GLFWwindow* window)
{
  int windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0;
  glfwGetWindowPos(window, &windowX, &windowY);
  glfwGetWindowSize(window, &windowWidth, &windowHeight);

  int count = 0;
  GLFWmonitor** monitors = glfwGetMonitors(&count);
  GLFWmonitor* best = glfwGetPrimaryMonitor();
  int bestOverlap = 0;

  for (int i = 0; i < count; i++)
  {
    const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
    if (mode == nullptr)
      continue;

    int monitorX = 0, monitorY = 0;
    glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);

    const int overlapX = std::max(0, std::min(windowX + windowWidth, monitorX + mode->width) - std::max(windowX, monitorX));
    const int overlapY =
      std::max(0, std::min(windowY + windowHeight, monitorY + mode->height) - std::max(windowY, monitorY));
    const int overlap = overlapX * overlapY;

    if (overlap > bestOverlap)
    {
      bestOverlap = overlap;
      best = monitors[i];
    }
  }
  return best;
}
} // namespace

void MainWindow::SetFullscreen(bool fullscreen)
{
  if (fullscreen == mFullscreen || mWindow == nullptr)
    return;

  if (fullscreen)
  {
    // Remembered before the switch, because once it is fullscreen the window has no other size
    // to go back to.
    glfwGetWindowPos(mWindow, &mWindowedX, &mWindowedY);
    glfwGetWindowSize(mWindow, &mWindowedWidth, &mWindowedHeight);

    GLFWmonitor* monitor = MonitorForWindow(mWindow);
    const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
    if (monitor == nullptr || mode == nullptr)
      return;

    // The screen's own mode rather than a chosen one: no resolution change, so no black flash and
    // nothing for the audio device to trip over while the display re-syncs.
    glfwSetWindowMonitor(mWindow, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
  }
  else
  {
    // A saved size of zero means it started fullscreen and was never windowed; something sensible
    // is better than a window with no area.
    const int width = mWindowedWidth > 0 ? mWindowedWidth : 1280;
    const int height = mWindowedHeight > 0 ? mWindowedHeight : 800;
    glfwSetWindowMonitor(mWindow, nullptr, mWindowedX, mWindowedY, width, height, 0);
  }

  mFullscreen = fullscreen;
  mConfig.fullscreen = fullscreen;
  mConfig.Save();
}

void MainWindow::HandleShortcuts()
{
  // F11 works everywhere, dialog open or not, and whether or not a text box has focus - it is the
  // one key that is never part of what you are typing.
  if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
    SetFullscreen(!mFullscreen);

  // Don't steal keys while the user is typing in a search box.
  if (ImGui::GetIO().WantTextInput)
    return;

  // Nor while a dialog is up. Its own keys are its business, and Space toggling a block behind a
  // confirmation you are reading is exactly the sort of thing that makes a shortcut untrustworthy.
  if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    return;

  // The transport is on the screen in every view, so its keys work in every view. A player you can
  // start from anywhere but stop only from one screen is worse than no shortcut at all.
  HandleTransportShortcuts();

  if (mView == View::Player)
  {
    HandlePlayerShortcuts();
    return;
  }

  // Delete removes the selected block - through the same confirmation the X on the block opens,
  // never straight away. Not the gate (id 0) and not a routing point (negative), neither of which
  // is something you can remove.
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && mSelectedBlockId > 0)
    mPendingRemoveBlockId = mSelectedBlockId;

  // 1 selects the gate, 2 onwards walk the chain left to right.
  const auto blocks = mEngine.GetBlocks();
  if (ImGui::IsKeyPressed(ImGuiKey_1, false))
    mSelectedBlockId = 0;
  for (size_t index = 0; index < blocks.size() && index < 8; index++)
  {
    const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(index) + 1);
    if (ImGui::IsKeyPressed(key, false))
      mSelectedBlockId = blocks[index].id;
  }

  // B takes the selected unit in and out - the fastest way to hear what it is contributing. This
  // was Space, which now belongs to the transport in every view; bypass is what B means on every
  // pedal ever made, so it is not a worse home for it.
  if (ImGui::IsKeyPressed(ImGuiKey_B, false))
  {
    if (mSelectedBlockId == 0)
    {
      GateSettings gate = mEngine.GetGate();
      gate.enabled = !gate.enabled;
      mEngine.SetGate(gate);
      mConfig.gateEnabled = gate.enabled;
      mConfig.Save();
    }
    else
    {
      BlockSettings settings;
      if (mEngine.GetBlockSettings(mSelectedBlockId, settings))
      {
        settings.enabled = !settings.enabled;
        mEngine.SetBlockSettings(mSelectedBlockId, settings);
        SaveChainToConfig();
      }
    }
  }

  // The tuner used to walk its strings with the arrows. It finds the string by itself now, and the
  // arrows scrub the transport in every view, which is the more useful of the two.
  if (mView == View::Tuner)
    return;

  // Auditioning with the arrows only makes sense where the list is on screen.
  if (mView != View::Library || mVisiblePaths.empty())
    return;

  const int lastIndex = static_cast<int>(mVisiblePaths.size()) - 1;
  bool moved = false;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
  {
    mSelectedIndex = std::min(mSelectedIndex + 1, lastIndex);
    moved = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
  {
    mSelectedIndex = std::max(mSelectedIndex - 1, 0);
    moved = true;
  }

  if (moved || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
  {
    const auto& path = mVisiblePaths[static_cast<size_t>(mSelectedIndex)];
    // The details pane follows the arrows too - it tracks the selection, not just the last click.
    mSelectedCapturePath = path;
    if (mSelectedBlockId != 0)
      LoadFileIntoBlock(mSelectedBlockId, path);
  }
}

/// \brief The keys that belong to the transport, which is drawn under every view.
///
/// Split out from the Play view's own keys for exactly that reason. Space stopping the song in one
/// view and doing something else in another is the kind of shortcut nobody trusts enough to use.
void MainWindow::HandleTransportShortcuts()
{
  Player& player = mEngine.GetPlayer();
  const ImGuiIO& io = ImGui::GetIO();
  const double position = player.GetPositionSeconds();
  const double duration = player.DurationSeconds();

  // --- the project, on the keys every application uses for it ---
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
  {
    if (mProjectFile.empty() || io.KeyShift)
    {
      std::snprintf(mProjectNameInput, sizeof(mProjectNameInput), "%s",
                    mProjectName.empty() ? "New project" : mProjectName.c_str());
      mSaveProjectDialogOpen = true;
    }
    else
    {
      SaveProjectTo(mProjectFile.parent_path(), mProjectName);
    }
    return;
  }

  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
  {
    BrowseAndOpenProject();
    return;
  }

  if (io.KeyCtrl)
    return; // anything else with Ctrl held is not ours

  if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
    player.TogglePlay();

  // Home goes back to the top everywhere. Enter does too, but only in the Play view - in a list of
  // captures Enter loads the highlighted one, and that is the older meaning of the two.
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false)
      || (mView == View::Player
          && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))))
    player.SetPositionSeconds(0.0);

  // Stop, and go back to where you last put the playhead - not to the top. Working a passage means
  // starting it over, and starting it over from the beginning of the song is never what was meant.
  if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))
  {
    player.Stop();
    player.SetPositionSeconds(std::clamp(mLastSeekSeconds, 0.0, std::max(0.0, duration)));
  }

  // Held down, so holding an arrow scrubs rather than stepping once per press.
  const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
  const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);

  if (left || right)
  {
    auto loops = player.GetLoops();
    if (io.KeyShift && !loops.empty())
    {
      // Shift walks the loops: work a passage, move to the next one.
      const int active = player.GetActiveLoop();
      const int count = static_cast<int>(loops.size());
      const int next = right ? (active + 1) % count : (active <= 0 ? count - 1 : active - 1);
      player.SetActiveLoop(next);
      player.SetPositionSeconds(loops[static_cast<size_t>(next)].startSeconds);
      mSelectedLoop = next;
    }
    else
    {
      player.SetPositionSeconds(std::clamp(position + (right ? 5.0 : -5.0), 0.0, std::max(0.0, duration)));
    }
  }

  // --- what the rest of the transport's buttons do, on their own keys ---
  if (ImGui::IsKeyPressed(ImGuiKey_C, false) && mTempo.valid)
    mClickArmed = !mClickArmed;

  if (ImGui::IsKeyPressed(ImGuiKey_L, false))
  {
    // Off if a loop is running, otherwise back to the one you last touched.
    if (player.GetActiveLoop() >= 0)
    {
      player.SetActiveLoop(-1);
    }
    else
    {
      const auto loops = player.GetLoops();
      const int wanted = (mSelectedLoop >= 0 && static_cast<size_t>(mSelectedLoop) < loops.size()) ? mSelectedLoop
                         : loops.empty()                                                           ? -1
                                                                                                   : 0;
      player.SetActiveLoop(wanted);
    }
  }
}

/// The keys that only mean anything where the timeline is on screen.
void MainWindow::HandlePlayerShortcuts()
{
  Player& player = mEngine.GetPlayer();
  const double position = player.GetPositionSeconds();
  const double duration = player.DurationSeconds();

  if (ImGui::GetIO().KeyCtrl)
    return; // the transport handler has already dealt with the ones we own

  if (ImGui::IsKeyPressed(ImGuiKey_F, false))
    mFollowPlayhead = !mFollowPlayhead;

  // --- zoom, about the playhead rather than the pointer: the keys are for when your hands are on
  // the guitar and the mouse is nowhere near the screen ---
  if (ImGui::IsKeyPressed(ImGuiKey_G, true) || ImGui::IsKeyPressed(ImGuiKey_H, true))
  {
    const bool in = ImGui::IsKeyPressed(ImGuiKey_H, true);
    const double span = mViewDuration > 0.0 ? mViewDuration : duration;
    const double newSpan = std::clamp(span * (in ? 0.7 : 1.4), 0.05, std::max(0.05, duration));
    mViewStart = std::clamp(position - newSpan * 0.5, 0.0, std::max(0.0, duration - newSpan));
    mViewDuration = newSpan;
  }

  // --- the selected track ---
  if (mSelectedTrackId != 0 && (ImGui::IsKeyPressed(ImGuiKey_M, false) || ImGui::IsKeyPressed(ImGuiKey_S, false)))
  {
    const bool mute = ImGui::IsKeyPressed(ImGuiKey_M, false);
    for (const auto& track : player.GetTracks())
    {
      if (track.id != mSelectedTrackId)
        continue;
      Track updated = track;
      if (mute)
        updated.muted = !updated.muted;
      else
        updated.soloed = !updated.soloed;
      player.SetTrack(updated);
    }
  }
}

void MainWindow::Draw()
{
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  // No padding on the root: the rail runs to the window's own edge, and each region inside adds
  // whatever it needs.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();

  DrawNavRail();
  ImGui::SameLine(0.0f, 0.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
  ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();


  // The tuner only analyses while its view is open - no point spending cycles otherwise - and it
  // is also what silences the output, so both follow the view directly.
  const bool tuning = (mView == View::Tuner);
  mEngine.SetTunerEnabled(tuning);
  mEngine.SetMuted(tuning && mTunerMutesOutput);

  // The view gets everything except the transport's row. Every view is laid out inside this, so
  // none of them has to know the transport is there.
  ImGui::BeginChild("##viewarea", ImVec2(0, -kTransportHeight), false);
  switch (mView)
  {
    case View::Tuner: DrawTunerView(); break;
    case View::Player: DrawPlayerView(); break;
    case View::Metronome: DrawMetronomeView(); break;
    case View::Library: DrawLibraryView(); break;
    case View::Settings: DrawSettingsView(); break;
    case View::Rig:
    default: DrawRigView(); break;
  }
  ImGui::EndChild();

  // --- the transport, under every view ---
  //
  // It used to live inside the player, which meant leaving the song to go and tune, and coming
  // back to start it again. It belongs to the application.
  DrawTransportBar();

  // The status strip along the bottom is gone. It repeated the sample rate and buffer size that
  // Settings already states, and named the blocks that the Rig view already shows - a line of
  // text that was never read because nothing on it was ever news.

  ImGui::EndChild();
  ImGui::End();

  // A window of its own rather than a popup: you set a track's EQ while the song plays and while
  // you keep working, so nothing else should be blocked behind it.
  DrawTrackEqPopup();

  // Asked for from either the rig's own menu or the library panel, so it is owned by neither.
  DrawDeleteRigPopup();

  // Raised by the mark at the head of the rail, which is on screen in every view.
  DrawAboutPopup();

  HandleShortcuts();
  PumpLoads();

  // Reaps finished download threads and starts any that were waiting for a free slot.
  mTone3000.PumpDownloads();

  // A sign-in, or a refresh the server answered with a rotated token, changes what has to survive
  // a restart. Noticed here rather than signalled from the worker, so nothing writes to the config
  // off the UI thread.
  const std::string refreshToken = mTone3000.GetRefreshToken();
  if (refreshToken != mPersistedRefreshToken)
  {
    mPersistedRefreshToken = refreshToken;
    mConfig.tone3000RefreshTokenEncrypted = refreshToken.empty() ? std::string() : EncryptSecret(refreshToken);
    mConfig.Save();
  }

  if (mTone3000.ConsumeDownloadCompleted())
  {
    mCaptureLibrary.Refresh();
    mCaptureLibrary.SetUserTags(mConfig.userTags);
  }
}

void MainWindow::DrawNavRail()
{
  // A column down the left rather than a band across the top. Six destinations do not need the
  // full width of a screen, and the band was costing a hundred and twenty pixels of height on the
  // one view - the timeline - that wants every pixel of it.
  constexpr float kIconCell = 46.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Rail());
  ImGui::BeginChild("##rail", ImVec2(kNavRailWidth, 0), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();

  // --- the mark, at the head of the column ---
  //
  // Where a title bar would be if the window had one. It is also the way into what this is and what
  // it is built on, which is the one thing in the app with nowhere else to live.
  {
    // Large enough for three rings to be three rings. Below about twenty-six pixels the mark falls
    // back to its single-ring form, and the rail is the one place it is worth paying a few pixels
    // to keep the full one.
    constexpr float kLogoSize = 34.0f;

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##about", ImVec2(kNavRailWidth, kLogoSize));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
      mAboutOpen = true;
    if (hovered)
      ImGui::SetTooltip("About Woodshed");

    // The rings take the app's accent rather than the file's own orange, so there is one orange in
    // the window and not two that nearly match.
    theme::Logo(ImGui::GetWindowDrawList(), ImVec2(pos.x + kNavRailWidth * 0.5f, pos.y + kLogoSize * 0.5f),
                kLogoSize, ImGui::GetColorU32(theme::Accent()),
                ImGui::GetColorU32(hovered ? theme::Control() : theme::Wood()));

    // Air underneath, so the mark reads as what the column belongs to rather than as the first
    // thing on it.
    ImGui::Dummy(ImVec2(0.0f, 24.0f));
  }

  struct Entry
  {
    const char* id;
    NavIcon icon;
    View view;
    const char* name;
  };
  // The player first: it is where a session starts and where most of it is spent.
  static const Entry kEntries[] = {{"player", NavIcon::Player, View::Player, "Player"},
                                   {"rig", NavIcon::Rig, View::Rig, "Rig"},
                                   {"tuner", NavIcon::Tuner, View::Tuner, "Tuner"},
                                   {"metronome", NavIcon::Metronome, View::Metronome, "Metronome"},
                                   {"library", NavIcon::Library, View::Library, "Library"}};

  // The metronome's arm swings on the icon while the click is running, on the same phase and the
  // same side as the big pendulum - so wherever you are in the app, the rail is telling you the
  // tempo without having to go and look at it.
  const auto metronomeSwing = [this]()
  {
    if (!mMetronomeRunning)
      return 1.0f; // the lean it is drawn with when it is still

    constexpr float kPi = 3.14159265358979323846f;
    const float side = (mEngine.GetMetronomeBeat() % 2 == 0) ? 1.0f : -1.0f;
    return side * std::cos(mEngine.GetMetronomePhase() * kPi);
  };

  // --- clipping, held long enough to be seen ---
  //
  // A peak over the top lasts a few samples; at sixty frames a second most of them would fall
  // between two draws and never appear. Held for a fifth of a second, which is long enough to
  // notice and short enough that it stops as soon as you back off.
  //
  // Read from the chain rather than from the output, because the click is mixed in after the chain
  // and a metronome tick is a loud, short transient - it was setting the cone rattling on every
  // beat, which said the amp was breaking up when nothing was even plugged in.
  constexpr float kClipDb = -0.5f;
  constexpr double kClipHold = 0.2;
  if (mEngine.GetChainLevelDb() > kClipDb)
    mClipUntil = ImGui::GetTime() + kClipHold;
  const float clipping = (ImGui::GetTime() < mClipUntil) ? 1.0f : 0.0f;

  // --- how hard a string is ringing ---
  //
  // Above the floor there is a note to hear; by the top of the range it is a fresh strike. Below
  // the floor the fork is still, so the rail is quiet when nothing is being played - which is what
  // stops it from being decoration.
  constexpr float kQuietDb = -34.0f;
  constexpr float kLoudDb = -12.0f;
  const float ringing = std::clamp((mEngine.GetInputLevelDb() - kQuietDb) / (kLoudDb - kQuietDb), 0.0f, 1.0f);

  const bool playing = mEngine.GetPlayer().IsPlaying();

  // --- downloads that finished while you were somewhere else ---
  //
  // You start a download in the library and walk away from it; the useful thing is being told when
  // it is done, not watching something move while you are already looking at it. Cleared by going
  // back to the library, which is where you would go to use what arrived.
  const bool downloading = mTone3000.IsDownloading();
  if (mWasDownloading && !downloading && mView != View::Library)
    mDownloadsWaiting = true;
  mWasDownloading = downloading;
  if (mView == View::Library)
    mDownloadsWaiting = false;

  for (const auto& entry : kEntries)
  {
    NavState state;
    // The player's mouth: opening and shutting a couple of times a second while it is singing, and
    // shut the rest of the time - where the icon is exactly the play triangle it always was.
    const float mouth = playing ? 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 13.0f) : 0.0f;
    state.swing = (entry.icon == NavIcon::Metronome) ? metronomeSwing()
                  : (entry.icon == NavIcon::Player)  ? mouth
                                                     : 1.0f;
    state.shake = (entry.icon == NavIcon::Rig)     ? clipping
                  : (entry.icon == NavIcon::Tuner) ? ringing
                                                   : 0.0f;
    state.playing = (entry.icon == NavIcon::Player) && playing;
    if (entry.icon == NavIcon::Library && mDownloadsWaiting)
    {
      state.badge = true;
      state.badgeColour = theme::Accent();
    }

    if (NavRailButton(entry.id, entry.icon, mView == entry.view, kNavRailWidth, kIconCell, state))
      mView = entry.view;
    if (ImGui::IsItemHovered())
    {
      if (entry.icon == NavIcon::Rig && state.shake > 0.0f)
        ImGui::SetTooltip("%s - clipping", entry.name);
      else if (entry.icon == NavIcon::Library && mDownloadsWaiting)
        ImGui::SetTooltip("%s - downloads finished", entry.name);
      else
        ImGui::SetTooltip("%s", entry.name);
    }
  }

  // --- what goes in and what comes out, at the foot of the column ---
  //
  // These are the only three controls that mean the same thing in every view, so they belong with
  // the navigation rather than in a band of their own across the top of whichever view is up.
  // Gate, then in, then out: the order the signal meets them. The meters that used to sit under
  // the two gain knobs are gone - the knob's own arc carries the level now, which is one object
  // saying one thing instead of two objects saying halves of it.
  constexpr float kKnobSize = 38.0f;

  // Placed from the bottom edge upwards rather than by leaving a gap and hoping. Estimating the
  // space these need meant adding up item heights and forgetting that ImGui puts spacing between
  // every one of them - which came to about thirty pixels of overflow, and pushed the settings
  // button off the end of a rail that has no scrollbar to find it with.
  const float knobItem = kKnobSize + ImGui::GetTextLineHeight() + 6.0f;
  const float railHeight = ImGui::GetWindowHeight();

  const float settingsY = railHeight - kIconCell - 10.0f;
  const float outY = settingsY - knobItem - 14.0f;
  const float inY = outY - knobItem - 10.0f;
  const float gateY = inY - knobItem - 10.0f;

  const auto centred = [&](float itemWidth, float y)
  {
    ImGui::SetCursorPosY(y);
    ImGui::SetCursorPosX((kNavRailWidth - itemWidth) * 0.5f);
  };

  // --- the gate ---
  //
  // One knob and nothing else. It used to have a power button and a pre/post switch beside it,
  // which is three controls for a thing with one useful setting: gating after the amp gates the
  // amp's own noise back in, so there was never a reason to choose post - and a threshold wound
  // all the way down is already "off", so the switch was saying what the knob had already said.
  GateSettings gate = mEngine.GetGate();
  const GateSettings before = gate;

  float threshold = gate.enabled ? gate.thresholdDb : 0.0f;
  const bool off = threshold >= -0.5f;

  centred(kKnobSize, gateY);
  // A format string with no conversion in it prints itself, which is how the readout says "off"
  // rather than "0" at the top of the travel.
  theme::Knob("GATE", &threshold, -90.0f, 0.0f, 0.0f, off ? "off" : "%.0f", kKnobSize, nullptr, 0, false);
  const bool gateReleased = theme::KnobReleased();

  gate.enabled = threshold < -0.5f;
  if (gate.enabled)
    gate.thresholdDb = threshold;
  gate.placement = GatePlacement::Pre;

  if (gate.enabled != before.enabled || gate.thresholdDb != before.thresholdDb
      || gate.placement != before.placement)
    mEngine.SetGate(gate);
  if (gateReleased)
  {
    mConfig.gateEnabled = gate.enabled;
    mConfig.gatePlacement = static_cast<int>(gate.placement);
    mConfig.gateThresholdDb = gate.thresholdDb;
    mConfig.Save();
  }

  centred(kKnobSize, inY);
  const ImVec4 inColour = theme::LevelColour(mEngine.GetInputLevelDb());
  float inputGain = mEngine.GetInputGainDb();
  if (theme::Knob("IN", &inputGain, -24.0f, 24.0f, 0.0f, "%.1f", kKnobSize, nullptr, 0, false, &inColour))
  {
    mEngine.SetInputGainDb(inputGain);
    mConfig.inputGainDb = inputGain;
  }
  if (theme::KnobReleased())
    mConfig.Save();

  centred(kKnobSize, outY);
  const ImVec4 outColour = theme::LevelColour(mEngine.GetOutputLevelDb());
  float outputGain = mEngine.GetOutputGainDb();
  if (theme::Knob("OUT", &outputGain, -24.0f, 24.0f, 0.0f, "%.1f", kKnobSize, nullptr, 0, false, &outColour))
  {
    mEngine.SetOutputGainDb(outputGain);
    mConfig.outputGainDb = outputGain;
  }
  if (theme::KnobReleased())
    mConfig.Save();

  // Settings is not a destination like the others - it is where you go to set the thing up rather
  // than to use it - so it sits apart, at the far end of the column.
  ImGui::SetCursorPosY(settingsY);
  ImGui::SetCursorPosX(0.0f);
  // A dot, not a spinning gear. Nothing in Settings runs, so there is nothing to animate - but a
  // stream that is not open is worth saying from wherever you happen to be, because every other
  // symptom of it is silence, which looks like a hundred other things.
  NavState settingsState;
  settingsState.badge = !mEngine.IsOpen();
  settingsState.badgeColour = theme::Warning();

  if (NavRailButton("settings", NavIcon::Settings, mView == View::Settings, kNavRailWidth, kIconCell, settingsState))
    mView = View::Settings;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(settingsState.badge ? "Settings - no audio device open" : "Settings");

  ImGui::EndChild();
}

void MainWindow::DrawRigView()
{
  // Fills what it is given. The transport's row is already taken out one level up.
  const float bodyHeight = 0.0f;

  // The meters and global controls live in the header now, so the chain gets the full width.
  ImGui::BeginChild("##units", ImVec2(0, bodyHeight), true);

  // What is loaded, above what it is made of - the same place the project name sits in the Play
  // view and the preset in the metronome.
  DrawRigMenu();

  DrawBlockRail();
  ImGui::Separator();

  // The routing points can vanish under the selection - removing the last block on the lower
  // branch dissolves the section - so drop the selection rather than draw a panel for nothing.
  // The two ends of the chain cannot vanish, so they are not subject to this.
  if ((mSelectedBlockId == kSplitPointSelection || mSelectedBlockId == kMergePointSelection)
      && mSelectedSection >= mEngine.GetRouting().sectionCount)
    mSelectedBlockId = 0;

  ImGui::BeginChild("##unitbody", ImVec2(0, 0), false);
  if (mSelectedBlockId == kMergePointSelection || mSelectedBlockId == kSplitPointSelection)
  {
    DrawMergePanel();
  }
  else if (mSelectedBlockId == kInputSelection || mSelectedBlockId == kOutputSelection)
  {
    DrawEndpointPanel(mSelectedBlockId == kInputSelection);
  }
  else if (mSelectedBlockId > 0)
  {
    DrawBlockPanel(mSelectedBlockId);
  }
  else
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("Select a block above, or add one with +");
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();

  ImGui::EndChild();

  // Opened outside the panel it was triggered from: the panel disappears with the block.
  DrawRemoveBlockPopup();
}
void MainWindow::DrawRemoveBlockPopup()
{
  if (mPendingRemoveBlockId == 0)
    return;

  if (!ImGui::IsPopupOpen("Remove block"))
    ImGui::OpenPopup("Remove block");

  if (ImGui::BeginPopupModal("Remove block", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    const auto found = mBlockPaths.find(mPendingRemoveBlockId);
    if (found != mBlockPaths.end() && !found->second.empty())
      ImGui::Text("Remove this block and unload %s?", found->second.stem().string().c_str());
    else
      ImGui::TextUnformatted("Remove this block?");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    const bool confirmed = ImGui::Button("Remove", ImVec2(140, 0));
    ImGui::PopStyleColor();
    ImGui::SetItemDefaultFocus();

    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel", ImVec2(140, 0));

    // Enter confirms. Nothing leaves the disk here - the block goes, whatever was loaded into it
    // stays in the library - so the quick way through is the right default.
    const bool acceptedByKey = ImGui::IsKeyPressed(ImGuiKey_Enter, false)
                               || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);

    if (confirmed || acceptedByKey)
    {
      RemoveBlock(mPendingRemoveBlockId);
      mPendingRemoveBlockId = 0;
      ImGui::CloseCurrentPopup();
    }
    else if (cancelled)
    {
      mPendingRemoveBlockId = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  else
  {
    // Dismissed some other way, e.g. Escape.
    mPendingRemoveBlockId = 0;
  }
}

void MainWindow::SaveRoutingToConfig()
{
  const ChainRouting routing = mEngine.GetRouting();
  mConfig.sections.clear();
  for (size_t s = 0; s < routing.sectionCount; s++)
  {
    const ParallelSection& section = routing.sections[s];
    AppConfig::SectionConfig stored;
    stored.splitIndex = static_cast<int>(section.splitIndex);
    stored.mergeIndex = static_cast<int>(section.mergeIndex);
    stored.mode = static_cast<int>(section.mode);
    stored.crossoverHz = section.crossoverHz;
    stored.upperDb = section.upperDb;
    stored.lowerDb = section.lowerDb;
    stored.mergeLevelDb = section.mergeLevelDb;
    mConfig.sections.push_back(stored);
  }
  mConfig.Save();
}

void MainWindow::DrawBlockRail()
{
  const auto blocks = mEngine.GetBlocks();
  const bool bypassed = mEngine.IsBypassed();
  const ChainRouting routing = mEngine.GetRouting();

  const float railWidth = ImGui::GetContentRegionAvail().x;
  // The share of the view the chain gets. It was half, capped at 320 pixels, which on any real
  // window left the picture small and a band of nothing underneath it. This is the thing the view
  // exists for, so it takes the larger part of it.
  const float railHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.56f, 300.0f, 560.0f);

  // Scrolls sideways when the chain outgrows the window. It could not before, because the columns
  // were always divided into whatever width there was - which was fine at eight blocks and would
  // have drawn twenty-four of them on top of each other.
  ImGui::BeginChild("##rail", ImVec2(0, railHeight), false,
                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_HorizontalScrollbar);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // Two lanes, not three: the line runs straight through, and a parallel branch hangs under it.
  // A spine with one branch above and one below meant the signal went up, along, and back down
  // again to reach the next block on the main line - three lanes to draw a fork that has two
  // sides. The lower lane doubles as a drop target whether or not anything is on it yet, which is
  // what makes "drag a block below the line to go parallel" discoverable.
  const float laneGap = std::clamp(railHeight * 0.30f, 140.0f, 210.0f);
  const float lanesHeight = (routing.sectionCount > 0) ? laneGap : 0.0f;
  // Centred as a group, so adding a branch grows the picture from the middle instead of leaving
  // the lanes parked at the top with the rest of the rail empty. Nudged up by the room a name
  // takes under the lowest lane.
  const float lineY = origin.y + (railHeight - lanesHeight) * 0.5f - 14.0f;
  const float upperY = lineY;
  const float lowerY = lineY + laneGap;

  // --- layout ------------------------------------------------------------------------------
  //
  // The chain is one flat list; a parallel section is a range of it, dealt out between the two
  // lanes by each block's row. Columns are counted per branch inside a section, so a section is
  // as wide as its longer branch and the two lanes line up column for column - rather than the
  // section stretching out to the sum of both branches, with the lanes staggered.
  std::array<int, kMaxBlocks> columnOf{};
  std::array<int, kMaxBlocks> rowOf{};
  std::array<float, kMaxBlocks> laneYOf{};
  std::array<int, kMaxParallelSections> sectionStartColumn{};
  std::array<int, kMaxParallelSections> sectionEndColumn{};

  const size_t blockCount = std::min(blocks.size(), kMaxBlocks);
  int column = 0;
  size_t laidOut = 0;
  for (size_t s = 0; s < routing.sectionCount; s++)
  {
    for (; laidOut < routing.sections[s].splitIndex && laidOut < blockCount; laidOut++)
    {
      columnOf[laidOut] = column++;
      rowOf[laidOut] = 0;
      laneYOf[laidOut] = lineY;
    }

    sectionStartColumn[s] = column;
    int upper = column;
    int lower = column;
    for (; laidOut < routing.sections[s].mergeIndex && laidOut < blockCount; laidOut++)
    {
      const bool onLower = blocks[laidOut].settings.row == 1;
      rowOf[laidOut] = onLower ? 1 : 0;
      columnOf[laidOut] = onLower ? lower++ : upper++;
      laneYOf[laidOut] = onLower ? lowerY : upperY;
    }
    column = std::max(upper, lower);
    sectionEndColumn[s] = column;
  }
  for (; laidOut < blockCount; laidOut++)
  {
    columnOf[laidOut] = column++;
    rowOf[laidOut] = 0;
    laneYOf[laidOut] = lineY;
  }

  const bool canAdd = blocks.size() < kMaxBlocks;
  // No trailing column for an add button any more; the columns are the blocks.
  const int columnCount = column;

  // Wide enough that the first and last blocks clear the IN and OUT nodes on the ends of the line,
  // and that the names under those nodes have somewhere to go.
  const float sideMargin = std::clamp(railWidth * 0.075f, 96.0f, 160.0f);
  const float firstX = origin.x + sideMargin;
  const float span = railWidth - sideMargin * 2.0f;
  // One size, always. It used to be derived from the column spacing and capped at 56, so a block
  // shrank as the chain grew and four of them across a wide window came out as small squares far
  // apart. A block you have to squint at is not a block you can drag with any confidence, and the
  // lane gap above is set with this number in mind rather than the other way round.
  constexpr float kNodeSize = 52.0f;
  // Spread across the width while they fit; past that they pack at a spacing that keeps the names
  // under them apart, and the rail scrolls.
  constexpr float kMinSpacing = kNodeSize + 46.0f;
  const float nodeSpacing =
    columnCount > 1 ? std::max(kMinSpacing, span / static_cast<float>(columnCount - 1)) : span;
  const float contentWidth = sideMargin * 2.0f + nodeSpacing * static_cast<float>(std::max(0, columnCount - 1));

  const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_Border);

  const auto nodeCentreX = [&](int position) { return firstX + static_cast<float>(position) * nodeSpacing; };
  // Kept inside the rail: at column 0 the natural point sits half a gap left of the first node,
  // which for a wide spacing lands outside the panel and cannot be grabbed at all.
  const auto pointX = [&](int position)
  {
    return std::clamp(nodeCentreX(position) - nodeSpacing * 0.5f, origin.x + 14.0f,
                      origin.x + contentWidth - 14.0f);
  };

  // The spine, drawn only where the signal is actually on it. Between a split and a merge it has
  // left the line for the two branches, and running it straight through would say the opposite.
  // Drawn before the nodes so it sits behind them; the handles that move the forks are placed
  // after the nodes, so they win the hover where the two overlap.
  float penX = origin.x + 6.0f;
  for (size_t s = 0; s < routing.sectionCount; s++)
  {
    const float splitX = pointX(sectionStartColumn[s]);
    const float mergeX = pointX(sectionEndColumn[s]);

    draw->AddLine(ImVec2(penX, lineY), ImVec2(splitX, lineY), lineColor, 2.0f);

    // The fork: one branch up, one down, back together at the merge.
    draw->AddLine(ImVec2(splitX, upperY), ImVec2(splitX, lowerY), lineColor, 2.0f);
    draw->AddLine(ImVec2(splitX, upperY), ImVec2(mergeX, upperY), lineColor, 2.0f);
    draw->AddLine(ImVec2(splitX, lowerY), ImVec2(mergeX, lowerY), lineColor, 2.0f);
    draw->AddLine(ImVec2(mergeX, upperY), ImVec2(mergeX, lowerY), lineColor, 2.0f);

    penX = mergeX;
  }
  draw->AddLine(ImVec2(penX, lineY), ImVec2(origin.x + contentWidth - 6.0f, lineY), lineColor, 2.0f);

  // --- nodes ---

  /// What a click on a node landed on. A block carries its own switch and remove button, so the
  /// square is three targets rather than one.
  enum class NodeHit
  {
    None,
    Body,
    Power,
    Remove
  };

  const auto drawNode = [&](const char* nodeId, int position, float rowY, const char* badge, const std::string& name,
                            ImVec4 color, bool selected, bool lit, bool controls, bool enabled,
                            float levelDb) -> NodeHit
  {
    const float centerX = nodeCentreX(position);
    const ImVec2 boxMin = ImVec2(centerX - kNodeSize * 0.5f, rowY - kNodeSize * 0.5f);
    const ImVec2 boxMax = ImVec2(boxMin.x + kNodeSize, boxMin.y + kNodeSize);

    const ImVec4 body = lit ? color : theme::TextDim();
    draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImVec4(body.x, body.y, body.z, lit ? 0.22f : 0.10f)), 6.0f);
    draw->AddRect(boxMin, boxMax, ImGui::GetColorU32(selected ? ImVec4(1, 1, 1, 0.9f) : body), 6.0f, 0,
                  selected ? 2.0f : 1.4f);

    NodeHit hit = NodeHit::None;

    // The corner controls only appear under the pointer, so the line stays quiet until you reach
    // for something. The hover test has to be the geometric one: the controls are submitted before
    // the body item that would otherwise be the thing reporting the hover. That order is also what
    // keeps a click on a corner from hitting the square underneath it, or starting a drag - ImGui
    // hands the hover to the first item that claims it.
    const bool pointerOnNode = controls && mDraggingBlockId == 0 && ImGui::IsWindowHovered()
                               && ImGui::IsMouseHoveringRect(boxMin, boxMax);
    if (pointerOnNode)
    {
      const float corner = std::clamp(kNodeSize * 0.34f, 18.0f, 28.0f);
      constexpr float kInset = 2.0f;
      const ImU32 chrome = ImGui::GetColorU32(theme::Surface());

      const ImVec2 removeMin = ImVec2(boxMin.x + kInset, boxMin.y + kInset);
      draw->AddRectFilled(removeMin, ImVec2(removeMin.x + corner, removeMin.y + corner), chrome, 4.0f);
      ImGui::SetCursorScreenPos(removeMin);
      if (theme::CloseButton("remove", corner))
        hit = NodeHit::Remove;

      const ImVec2 powerMin = ImVec2(boxMax.x - kInset - corner, boxMin.y + kInset);
      draw->AddRectFilled(powerMin, ImVec2(powerMin.x + corner, powerMin.y + corner), chrome, 4.0f);
      ImGui::SetCursorScreenPos(powerMin);
      bool on = enabled;
      if (theme::PowerButton("blockpower", &on, corner))
        hit = NodeHit::Power;
    }

    // Submitted last, so the caller's IsItemActive() - the drag test - refers to the square itself.
    ImGui::SetCursorScreenPos(boxMin);
    ImGui::InvisibleButton(nodeId, ImVec2(kNodeSize, kNodeSize));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
      hit = NodeHit::Body;

    const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
    draw->AddText(ImVec2(centerX - badgeSize.x * 0.5f, rowY - badgeSize.y * 0.5f), ImGui::GetColorU32(body), badge);

    // The level leaving this block. The engine has always measured it; without somewhere to show
    // it, whether one block is really feeding the next is something you can only take on trust.
    if (levelDb > -100.0f)
    {
      constexpr float kMeterHeight = 3.0f;
      const float meterTop = boxMax.y + 3.0f;
      draw->AddRectFilled(ImVec2(boxMin.x, meterTop), ImVec2(boxMax.x, meterTop + kMeterHeight),
                          ImGui::GetColorU32(ImGuiCol_FrameBg), 1.5f);

      const float fill = std::clamp((levelDb + 60.0f) / 60.0f, 0.0f, 1.0f);
      if (fill > 0.0f)
      {
        const ImVec4 meterColor = (levelDb > -1.0f) ? theme::Danger() : body;
        draw->AddRectFilled(ImVec2(boxMin.x, meterTop),
                            ImVec2(boxMin.x + (boxMax.x - boxMin.x) * fill, meterTop + kMeterHeight),
                            ImGui::GetColorU32(meterColor), 1.5f);
      }
    }

    const float nameWidth = std::max(60.0f, nodeSpacing - 10.0f);
    const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
    const float nameX = centerX - std::min(nameSize.x, nameWidth) * 0.5f;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.82f, ImVec2(nameX, boxMax.y + 10.0f),
                  ImGui::GetColorU32(lit ? body : theme::TextDim()), name.c_str(), nullptr, nameWidth);

    if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
      ImGui::SetTooltip("%s", name.empty() ? badge : name.c_str());
    return hit;
  };

  for (size_t index = 0; index < blockCount; index++)
  {
    const Block& block = blocks[index];
    const auto found = mBlockPaths.find(block.id);
    const bool loading = mCaptureLibrary.IsLoading(block.id);

    // Only a block that loads a file can be empty. A cut, a compressor or an EQ is complete as
    // soon as it exists, and the badge in the square already says which it is.
    const bool loadsAFile =
      (block.settings.type == BlockType::Nam || block.settings.type == BlockType::Ir);

    std::string name;
    if (loading)
      name = "loading...";
    else if (found != mBlockPaths.end() && !found->second.empty())
      name = Shorten(found->second.stem().string(), 14);
    else if (loadsAFile)
      name = "empty";

    ImGui::PushID(block.id);
    const NodeHit hit = drawNode("node", columnOf[index], laneYOf[index], BlockTypeName(block.settings.type), name,
                                 BlockColor(block.settings.type), block.id == mSelectedBlockId,
                                 block.settings.enabled && !bypassed, true, block.settings.enabled,
                                 mEngine.GetBlockLevelDb(index));

    if (hit == NodeHit::Body)
    {
      mSelectedBlockId = block.id;
    }
    else if (hit == NodeHit::Power)
    {
      BlockSettings settings = block.settings;
      settings.enabled = !settings.enabled;
      mEngine.SetBlockSettings(block.id, settings);
      SaveChainToConfig();
    }
    else if (hit == NodeHit::Remove)
    {
      // The confirmation is drawn at the end of the view; setting this is what opens it.
      mPendingRemoveBlockId = block.id;
    }

    // Dragging is handled directly rather than through ImGui's drag-drop, because the gesture
    // carries two meanings at once: where along the chain, and which branch.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
      mDraggingBlockId = block.id;

    ImGui::PopID();
  }

  // No add button. A block goes where you put it, so the gesture is a right-click on the place you
  // want it - which is also the only way to say "here" rather than "at the end", and the reason a
  // button parked at the right-hand end could never express it.

  // Which column the pointer is nearest, and what the boundary left of that column stands for in
  // the flat chain. Columns and chain positions are not the same thing once a section stacks two
  // branches into one column, so every gesture goes through this.
  const auto columnUnderMouse = [&](float bias)
  {
    const long candidate = std::lround((ImGui::GetIO().MousePos.x - firstX) / nodeSpacing + bias);
    return static_cast<int>(std::clamp<long>(candidate, 0, columnCount));
  };

  const auto flatIndexAtColumn = [&](int boundary)
  {
    size_t flat = 0;
    for (size_t i = 0; i < blockCount; i++)
      if (columnOf[i] < boundary)
        flat++;
    return flat;
  };

  // --- split and merge handles, on top of the nodes so they are always grabbable ---
  {
    // A small square on the line, the shape a connector has on a schematic. It was a bar the full
    // height of the fork, which drew the loudest thing on the screen down the middle of the
    // picture - and the bar is already there as the fork's own line, in the colour a wire has.
    const auto handle = [&](const char* id, float x, const char* label, bool selected) -> bool
    {
      constexpr float kGrab = 26.0f; // what the pointer has to hit, larger than what is drawn
      constexpr float kSquare = 15.0f;

      ImGui::SetCursorScreenPos(ImVec2(x - kGrab * 0.5f, lineY - kGrab * 0.5f));
      ImGui::InvisibleButton(id, ImVec2(kGrab, kGrab));
      const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();

      const ImU32 colour = ImGui::GetColorU32(selected || hot ? theme::Control() : theme::TextDim());
      const ImVec2 boxMin(x - kSquare * 0.5f, lineY - kSquare * 0.5f);
      const ImVec2 boxMax(x + kSquare * 0.5f, lineY + kSquare * 0.5f);

      // Filled in the background colour first, so the wire does not run through the middle of it.
      draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(theme::Base()), 2.0f);
      draw->AddRect(boxMin, boxMax, colour, 2.0f, 0, 1.6f);
      // Two strokes for a grip: what says this one can be taken hold of and slid along.
      draw->AddLine(ImVec2(x - 2.5f, lineY - 4.0f), ImVec2(x - 2.5f, lineY + 4.0f), colour, 1.2f);
      draw->AddLine(ImVec2(x + 2.5f, lineY - 4.0f), ImVec2(x + 2.5f, lineY + 4.0f), colour, 1.2f);

      const ImVec2 labelSize = ImGui::CalcTextSize(label);
      draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.82f,
                    ImVec2(x - labelSize.x * 0.41f, lineY - kSquare * 0.5f - 19.0f),
                    ImGui::GetColorU32(hot ? theme::TextDim() : theme::TextFaint()), label);

      if (hot)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

      return ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    };

    for (size_t s = 0; s < routing.sectionCount; s++)
    {
      ImGui::PushID(static_cast<int>(s));

      const bool splitSelected = (mSelectedBlockId == kSplitPointSelection && mSelectedSection == s);
      const bool splitDragging = handle("splithandle", pointX(sectionStartColumn[s]), "split", splitSelected);
      if (ImGui::IsItemClicked())
      {
        mSelectedBlockId = kSplitPointSelection;
        mSelectedSection = s;
      }
      if (splitDragging)
      {
        const size_t wanted = flatIndexAtColumn(columnUnderMouse(0.5f));
        if (wanted != routing.sections[s].splitIndex)
        {
          ChainRouting moved = routing;
          moved.sections[s].splitIndex = wanted;
          // Dragging the split past the merge pushes the merge along instead of refusing to move.
          if (moved.sections[s].mergeIndex <= wanted)
            moved.sections[s].mergeIndex = std::min(wanted + 1, blockCount);
          if (moved.sections[s].mergeIndex > moved.sections[s].splitIndex)
          {
            mEngine.SetRouting(moved);
            SaveRoutingToConfig();
          }
        }
      }

      const bool mergeSelected = (mSelectedBlockId == kMergePointSelection && mSelectedSection == s);
      const bool mergeDragging = handle("mergehandle", pointX(sectionEndColumn[s]), "merge", mergeSelected);
      if (ImGui::IsItemClicked())
      {
        mSelectedBlockId = kMergePointSelection;
        mSelectedSection = s;
      }
      if (mergeDragging)
      {
        const size_t wanted = flatIndexAtColumn(columnUnderMouse(0.5f));
        if (wanted != routing.sections[s].mergeIndex)
        {
          ChainRouting moved = routing;
          moved.sections[s].mergeIndex = wanted;
          if (moved.sections[s].splitIndex >= wanted)
            moved.sections[s].splitIndex = (wanted > 0) ? wanted - 1 : 0;
          if (moved.sections[s].mergeIndex > moved.sections[s].splitIndex)
          {
            mEngine.SetRouting(moved);
            SaveRoutingToConfig();
          }
        }
      }

      ImGui::PopID();
    }
  }

  // --- resolve a drag ---
  if (mDraggingBlockId != 0)
  {
    const int targetColumn = std::min(columnUnderMouse(0.0f), std::max(0, columnCount - 1));
    const int targetRow = (ImGui::GetIO().MousePos.y > lineY + laneGap * 0.5f) ? 1 : 0;

    // The flat position that column stands for: the block nearest it, preferring the lane the
    // pointer is in, since inside a section one column can hold a block on each lane.
    size_t targetIndex = 0;
    int nearest = std::numeric_limits<int>::max();
    for (size_t i = 0; i < blockCount; i++)
    {
      const int distance = std::abs(columnOf[i] - targetColumn) * 2 + (rowOf[i] == targetRow ? 0 : 1);
      if (distance < nearest)
      {
        nearest = distance;
        targetIndex = i;
      }
    }

    // A ghost under the cursor, so the drop lands where it looks like it will. On the lane it
    // would actually end up on: above the spine only if it is landing inside a section, since
    // outside one there is no upper branch to be on.
    bool insideSection = false;
    for (size_t s = 0; s < routing.sectionCount; s++)
      if (targetColumn >= sectionStartColumn[s] && targetColumn < sectionEndColumn[s])
        insideSection = true;

    // Under a column, or in the gap between two? On the lower lane those mean different things -
    // alongside that block, or a branch of its own - so the ghost is drawn where it would land.
    const float centreOffset = ImGui::GetIO().MousePos.x - nodeCentreX(targetColumn);
    const bool droppedInGap = targetRow == 1 && std::fabs(centreOffset) > nodeSpacing * 0.34f;
    const float ghostX =
      nodeCentreX(targetColumn) + (droppedInGap ? std::copysign(nodeSpacing * 0.5f, centreOffset) : 0.0f);

    const float ghostY = (targetRow == 1) ? lowerY : (insideSection ? upperY : lineY);
    draw->AddRect(ImVec2(ghostX - kNodeSize * 0.5f, ghostY - kNodeSize * 0.5f),
                  ImVec2(ghostX + kNodeSize * 0.5f, ghostY + kNodeSize * 0.5f),
                  ImGui::GetColorU32(theme::Control()), 7.0f, 0, 1.5f);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      size_t from = 0;
      for (size_t i = 0; i < blockCount; i++)
        if (blocks[i].id == mDraggingBlockId)
          from = i;

      // Both read before the move, while the old layout still describes where things are.

      // Dropped between a split and a merge? Then it joins that section, on whichever lane it
      // landed on - that is what lets a branch hold more than one block.
      int joinAnchorId = 0;
      for (size_t s = 0; s < routing.sectionCount && joinAnchorId == 0; s++)
      {
        if (targetColumn < sectionStartColumn[s] || targetColumn >= sectionEndColumn[s])
          continue;
        for (size_t i = routing.sections[s].splitIndex; i < routing.sections[s].mergeIndex && i < blockCount; i++)
          if (blocks[i].id != mDraggingBlockId)
          {
            joinAnchorId = blocks[i].id;
            break;
          }
      }

      // Dropped opposite a block that is already there? Then it is meant to run alongside that
      // one, not merely land next to it.
      //
      // Unless it was dropped in the gap between two columns rather than under one. That is how a
      // branch of its own gets made: the line leaves the main path, goes through this block, and
      // comes straight back - with nothing opposite it. Without the distinction every drop on the
      // lower lane stretched a section over whatever block happened to be nearest, and a branch
      // that runs alone could not be expressed at all.
      int alongsideId = 0;
      if (!droppedInGap)
        for (size_t i = 0; i < blockCount; i++)
          if (columnOf[i] == targetColumn && rowOf[i] != targetRow && blocks[i].id != mDraggingBlockId)
            alongsideId = blocks[i].id;

      if (targetIndex != from)
      {
        mEngine.ReorderBlock(mDraggingBlockId, targetIndex);
        mLastMatchedCapture.clear();
      }
      // Order first, then lane: which section a block joins follows from where it landed.
      mEngine.SetBlockRow(mDraggingBlockId, targetRow);
      if (joinAnchorId != 0)
        mEngine.ExtendSectionOver(joinAnchorId, mDraggingBlockId);
      if (alongsideId != 0 && targetRow == 1)
        mEngine.ExtendSectionOver(mDraggingBlockId, alongsideId);

      SaveChainToConfig();
      SaveRoutingToConfig();

      mSelectedBlockId = mDraggingBlockId;
      mDraggingBlockId = 0;
    }
  }

  // --- right-click anywhere on the chain to put a block there ---
  //
  // Where "there" is: the boundary nearest the pointer, and the lane it is in. Remembered at the
  // moment of the click, because the popup opens on the next frame and the pointer has moved on by
  // the time anything is chosen from it.
  if (canAdd && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
  {
    mAddAtColumn = columnUnderMouse(0.5f);
    mAddAtIndex = static_cast<int>(flatIndexAtColumn(mAddAtColumn));
    // On the branch only when the pointer is actually on the branch's row. Anywhere in the lower
    // half of the rail used to count, and the rail is over half the window - so a right-click in
    // the empty space under the chain quietly put the block on a parallel branch of its own.
    mAddOnLowerLane = std::fabs(ImGui::GetIO().MousePos.y - lowerY) < kNodeSize;
    ImGui::OpenPopup("##addblock");
  }

  // Where it will land, drawn on the line while the menu is up: a caret at the boundary the click
  // chose, so the answer to "between which two" is on the screen rather than inferred afterwards.
  if (ImGui::IsPopupOpen("##addblock"))
  {
    const float caretX = pointX(mAddAtColumn);
    const float caretY = mAddOnLowerLane ? lowerY : lineY;
    const ImU32 caret = ImGui::GetColorU32(theme::Accent());
    draw->AddLine(ImVec2(caretX, caretY - 22.0f), ImVec2(caretX, caretY + 22.0f), caret, 2.0f);
    draw->AddTriangleFilled(ImVec2(caretX - 5.0f, caretY - 22.0f), ImVec2(caretX + 5.0f, caretY - 22.0f),
                            ImVec2(caretX, caretY - 14.0f), caret);
  }

  if (ImGui::BeginPopup("##addblock"))
  {
    theme::Hint(mAddOnLowerLane ? "On the branch, where the caret is" : "Where the caret is");
    ImGui::Separator();

    const auto add = [&](const char* label, BlockType type)
    {
      if (!ImGui::Selectable(label))
        return;

      const int id = AddBlock(type);
      if (id <= 0)
        return;

      // AddBlock appends; this is what puts it where the click was.
      const auto after = mEngine.GetBlocks();
      const size_t wanted = std::min(static_cast<size_t>(std::max(0, mAddAtIndex)), after.size() - 1);
      mEngine.ReorderBlock(id, wanted);
      if (mAddOnLowerLane)
        mEngine.SetBlockRow(id, 1);

      SaveChainToConfig();
      SaveRoutingToConfig();
      mSelectedBlockId = id;
    };

    add("NAM capture", BlockType::Nam);
    add("Cab impulse response", BlockType::Ir);
    add("Low / high cut", BlockType::Cut);
    add("Compressor", BlockType::Comp);
    add("Parametric EQ", BlockType::Eq);
    ImGui::EndPopup();
  }

  // --- where the chain starts and where it ends ---
  //
  // The line ran off both edges of the picture into nothing. These are the two ends of it, and
  // what they are set to - which input of the interface, and what trim - is a real part of the
  // rig rather than something that only exists in Settings.
  {
    const auto endpoint = [&](const char* id, float x, const char* label, bool rightAligned, bool selected,
                              float levelDb)
    {
      constexpr float kGrab = 26.0f;
      constexpr float kSquare = 15.0f;

      ImGui::SetCursorScreenPos(ImVec2(x - kGrab * 0.5f, lineY - kGrab * 0.5f));
      ImGui::InvisibleButton(id, ImVec2(kGrab, kGrab));
      const bool hot = ImGui::IsItemHovered();
      const bool clicked = ImGui::IsItemClicked();

      const ImU32 colour = ImGui::GetColorU32(selected || hot ? theme::Control() : theme::TextDim());
      const ImVec2 boxMin(x - kSquare * 0.5f, lineY - kSquare * 0.5f);
      const ImVec2 boxMax(x + kSquare * 0.5f, lineY + kSquare * 0.5f);
      draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(theme::Base()), 2.0f);
      draw->AddRect(boxMin, boxMax, colour, 2.0f, 0, 1.6f);

      // A short bar inside, coloured by what is passing through: this is the one place in the
      // picture where the level entering and leaving the whole chain can be seen.
      const float fill = std::clamp((levelDb + 60.0f) / 60.0f, 0.0f, 1.0f);
      if (fill > 0.0f)
        draw->AddRectFilled(ImVec2(x - 3.0f, lineY + 4.0f - 8.0f * fill), ImVec2(x + 3.0f, lineY + 4.0f),
                            ImGui::GetColorU32(theme::LevelColour(levelDb)), 1.0f);

      // Two letters, and nothing else. The device name and the trim were spelled out under each
      // node, which is a line of settings printed onto a diagram - what they are set to belongs in
      // the panel that sets them, and the tooltip says it for the moment you wonder.
      const float labelX = rightAligned ? x + kSquare * 0.5f - ImGui::CalcTextSize(label).x : x - kSquare * 0.5f;
      draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.82f, ImVec2(labelX, lineY - kSquare * 0.5f - 19.0f),
                    ImGui::GetColorU32(theme::TextDim()), label);

      return clicked;
    };

    const std::string inName = (mSelectedInputIndex >= 0 && mSelectedInputIndex < static_cast<int>(mInputDevices.size()))
                                 ? mInputDevices[static_cast<size_t>(mSelectedInputIndex)].name
                                 : std::string("nothing");
    if (endpoint("innode", origin.x + 14.0f, "IN", false, mSelectedBlockId == kInputSelection,
                 mEngine.GetInputLevelDb()))
      mSelectedBlockId = kInputSelection;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s, channel %d", inName.c_str(), mSelectedInputChannel + 1);

    const std::string outName =
      (mSelectedOutputIndex >= 0 && mSelectedOutputIndex < static_cast<int>(mOutputDevices.size()))
        ? mOutputDevices[static_cast<size_t>(mSelectedOutputIndex)].name
        : std::string("nothing");
    if (endpoint("outnode", origin.x + contentWidth - 14.0f, "OUT", true, mSelectedBlockId == kOutputSelection,
                 mEngine.GetOutputLevelDb()))
      mSelectedBlockId = kOutputSelection;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s, channels %d/%d", outName.c_str(), mSelectedOutputChannel + 1, mSelectedOutputChannel + 2);
  }

  // The chain's own switch is on the header row now, beside the rig's name, rather than floating
  // in the corner of the drawing it switches off.

  // The item that gives the child its scroll range, so a chain wider than the window can be
  // reached rather than merely clipped.
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(contentWidth, railHeight - 8.0f));

  ImGui::EndChild();
}

/// \brief What one end of the chain is connected to, and how hard it is driven.
///
/// The same three things at either end: the device, the level going through it, and the trim. It
/// was split between a knob in the rail and a combo in Settings, neither of which said it was the
/// start or the end of the chain drawn above.
void MainWindow::DrawEndpointPanel(bool isInput)
{
  const float levelDb = isInput ? mEngine.GetInputLevelDb() : mEngine.GetOutputLevelDb();

  // The same header row a block has, so selecting a node and selecting a block do not produce two
  // differently shaped panels.
  theme::ViewTitle(isInput ? "IN" : "OUT");
  ImGui::SameLine(0, 16);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
  theme::Hint(isInput ? "Where the chain starts" : "Where the chain ends");
  theme::Divider();

  const float height = ImGui::GetContentRegionAvail().y;
  const float deviceWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 320.0f, 640.0f);

  ImGui::BeginChild("##enddevice", ImVec2(deviceWidth, height), true);
  theme::SectionLabel(isInput ? "INPUT" : "OUTPUT");

  auto& devices = isInput ? mInputDevices : mOutputDevices;
  int& selected = isInput ? mSelectedInputIndex : mSelectedOutputIndex;

  const std::string current = (selected >= 0 && selected < static_cast<int>(devices.size()))
                                ? devices[static_cast<size_t>(selected)].name
                                : std::string("none");

  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##enddev", current.c_str()))
  {
    for (size_t i = 0; i < devices.size(); i++)
      if (ImGui::Selectable(devices[i].name.c_str(), static_cast<int>(i) == selected))
      {
        selected = static_cast<int>(i);
        ApplyDeviceSelection();
      }
    ImGui::EndCombo();
  }

  // --- which socket on it ---
  //
  // Picking the interface says nothing about where the guitar is plugged in. An eight-input box is
  // one device and eight places to listen to, and without this the app only ever heard the first
  // of them.
  if (selected >= 0 && selected < static_cast<int>(devices.size()))
  {
    const RtAudio::DeviceInfo& device = devices[static_cast<size_t>(selected)];
    const int available = static_cast<int>(isInput ? device.inputChannels : device.outputChannels);
    int& channel = isInput ? mSelectedInputChannel : mSelectedOutputChannel;

    ImGui::Spacing();
    theme::SectionLabel(isInput ? "CHANNEL" : "CHANNEL PAIR");

    // The output is a stereo pair, so the choice is which pair rather than which channel, and it
    // steps in twos.
    const int step = isInput ? 1 : 2;
    const int last = std::max(0, available - step);

    int shown = 0;
    for (int first = 0; first <= last; first += step)
    {
      if (shown > 0 && (shown % 8) != 0)
        ImGui::SameLine(0, 4);
      shown++;

      char label[16];
      if (isInput)
        std::snprintf(label, sizeof(label), "%d", first + 1);
      else
        std::snprintf(label, sizeof(label), "%d/%d", first + 1, first + 2);

      if (SegmentedButton(label, channel == first, isInput ? 44.0f : 58.0f) && channel != first)
      {
        channel = first;
        ApplyDeviceSelection();
      }
    }
    if (shown == 0)
      theme::Hint("This device has no channels of that kind.");

    char detail[96];
    std::snprintf(detail, sizeof(detail), "%d available   %u Hz   %u frames", available,
                  mEngine.GetActualSampleRate(), mEngine.GetActualBufferFrames());
    theme::Hint(detail);
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##endlevel", ImVec2(0, height), true);
  theme::SectionLabel("LEVEL");

  float gain = isInput ? mEngine.GetInputGainDb() : mEngine.GetOutputGainDb();

  if (BlockControl("endgain", "GAIN", &gain, -24.0f, 24.0f, 0.0f, "%+.1f dB", 96.0f))
  {
    if (isInput)
    {
      mEngine.SetInputGainDb(gain);
      mConfig.inputGainDb = gain;
    }
    else
    {
      mEngine.SetOutputGainDb(gain);
      mConfig.outputGainDb = gain;
    }
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();

  ImGui::Spacing();
  ImGui::BeginGroup();
  theme::LevelMeter("##endmeter", levelDb, std::max(160.0f, ImGui::GetContentRegionAvail().x - 20.0f));
  char reading[24];
  std::snprintf(reading, sizeof(reading), "%.1f dB", levelDb);
  theme::Value(reading, 1.0f);
  if (isInput)
    theme::Hint("Captures are level-sensitive. Land between -18 and -6 dB and they sound the way "
                "they were trained.");
  ImGui::EndGroup();

  ImGui::EndChild();
}

void MainWindow::DrawMergePanel()
{
  ChainRouting routing = mEngine.GetRouting();
  if (mSelectedSection >= routing.sectionCount)
    return;

  ParallelSection& section = routing.sections[mSelectedSection];
  const ParallelSection before = section;
  bool settled = false;

  theme::ViewTitle(mSelectedBlockId == kSplitPointSelection ? "SPLIT" : "MERGE");
  ImGui::SameLine(0, 16);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
  theme::Hint("Where the chain runs two ways at once");
  theme::Divider();

  const float height = ImGui::GetContentRegionAvail().y;
  constexpr float kLabelColumn = 106.0f;

  ImGui::BeginChild("##splitmode", ImVec2(std::max(360.0f, ImGui::GetContentRegionAvail().x * 0.44f), height), true);
  theme::SectionLabel("HOW IT SPLITS");
  {
    const bool isCrossover = (section.mode == SplitMode::Crossover);
    if (SegmentedButton("FULL", !isCrossover, 132.0f) && isCrossover)
    {
      section.mode = SplitMode::Full;
      settled = true;
    }
    ImGui::SameLine(0, 4);
    if (SegmentedButton("CROSSOVER", isCrossover, 132.0f) && !isCrossover)
    {
      section.mode = SplitMode::Crossover;
      settled = true;
    }

    theme::Hint(isCrossover ? "Lows go one way and highs the other, at the frequency below."
                            : "Both ways get the whole signal.");

    ImGui::BeginDisabled(!isCrossover);
    settled |=
      BlockControl("xover", "SPLIT AT", &section.crossoverHz, 60.0f, 4000.0f, 500.0f, "%.0f Hz", kLabelColumn);
    ImGui::EndDisabled();
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##splitmix", ImVec2(0, height), true);
  theme::SectionLabel("HOW MUCH OF EACH");
  {
    // Two levels, one per branch, and nothing between them. A single balance control could only
    // say how the two compare - moving one moved the other, it could not turn both down or push
    // both up, and "40 / 60" said nothing about how loud either branch actually was.
    settled |= BlockControl("mixa", "A  MAIN", &section.upperDb, -60.0f, 12.0f, 0.0f, "%+.1f dB", kLabelColumn);
    settled |= BlockControl("mixb", "B  BRANCH", &section.lowerDb, -60.0f, 12.0f, 0.0f, "%+.1f dB", kLabelColumn);

    theme::Divider();
    // Summing two branches usually changes the level, so the merge has its own trim to put the
    // chain back where it was.
    settled |=
      BlockControl("mergelevel", "OUT", &section.mergeLevelDb, -24.0f, 24.0f, 0.0f, "%+.1f dB", kLabelColumn);
  }
  ImGui::EndChild();

  if (section.mode != before.mode || section.crossoverHz != before.crossoverHz || section.upperDb != before.upperDb
      || section.lowerDb != before.lowerDb || section.mergeLevelDb != before.mergeLevelDb)
    mEngine.SetRouting(routing);
  if (settled)
    SaveRoutingToConfig();
}

bool MainWindow::DrawParametricEq(ParametricEqSettings& settings, float width, float height, bool showSpectrum)
{
  const double sampleRate = std::max(8000.0, static_cast<double>(mEngine.GetActualSampleRate()));
  const float nyquist = static_cast<float>(sampleRate * 0.5);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##eqplot", ImVec2(width, height));
  const bool plotHovered = ImGui::IsItemHovered();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 plotMax = ImVec2(origin.x + width, origin.y + height);
  // Black, like every other background. The plot used the field colour, which made it the one
  // grey rectangle left in the view.
  draw->AddRectFilled(origin, plotMax, ImGui::GetColorU32(theme::Base()), 6.0f);
  draw->PushClipRect(origin, plotMax, true);

  // Log frequency across, linear dB up. Both directions have a single mapping and its inverse,
  // and everything below goes through them so the curve, the grid and the handles cannot disagree.
  const float logMin = std::log10(kBandMinHz);
  const float logMax = std::log10(std::min(kBandMaxHz, nyquist));
  const auto xForHz = [&](float hz)
  { return origin.x + width * (std::log10(std::clamp(hz, kBandMinHz, nyquist)) - logMin) / (logMax - logMin); };
  const auto hzForX = [&](float x)
  { return std::pow(10.0f, logMin + (logMax - logMin) * std::clamp((x - origin.x) / width, 0.0f, 1.0f)); };

  constexpr float kPlotRangeDb = 20.0f;
  const auto yForDb = [&](float db)
  { return origin.y + height * (0.5f - std::clamp(db, -kPlotRangeDb, kPlotRangeDb) / (2.0f * kPlotRangeDb)); };
  const auto dbForY = [&](float y)
  { return (0.5f - std::clamp((y - origin.y) / height, 0.0f, 1.0f)) * 2.0f * kPlotRangeDb; };

  // --- grid ---
  const ImU32 gridColor = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f));
  const ImU32 gridText = ImGui::GetColorU32(theme::TextDim());
  static const float kGridHz[] = {50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f};
  for (float hz : kGridHz)
  {
    if (hz >= nyquist)
      continue;
    const float x = xForHz(hz);
    draw->AddLine(ImVec2(x, origin.y), ImVec2(x, plotMax.y), gridColor);
    char label[16];
    if (hz >= 1000.0f)
      std::snprintf(label, sizeof(label), "%gk", hz / 1000.0f);
    else
      std::snprintf(label, sizeof(label), "%g", hz);
    draw->AddText(ImVec2(x + 3.0f, plotMax.y - ImGui::GetTextLineHeight() - 2.0f), gridText, label);
  }
  for (float db : {-12.0f, 0.0f, 12.0f})
  {
    const float y = yForDb(db);
    draw->AddLine(ImVec2(origin.x, y), ImVec2(plotMax.x, y), db == 0.0f ? ImGui::GetColorU32(ImVec4(1, 1, 1, 0.14f))
                                                                       : gridColor);
  }

  // --- spectrum behind the curve ---
  if (showSpectrum)
    mEngine.GetSpectrum().Read(mSpectrumSamples);
  const size_t fftSize = showSpectrum ? mSpectrumSamples.size() : 0;
  if (fftSize >= 64)
  {
    mSpectrumReal.assign(mSpectrumSamples.begin(), mSpectrumSamples.end());
    mSpectrumImag.assign(fftSize, 0.0f);

    // Hann window: without it every note smears into a broad ridge of spectral leakage.
    for (size_t i = 0; i < fftSize; i++)
    {
      const float t = static_cast<float>(i) / static_cast<float>(fftSize - 1);
      mSpectrumReal[i] *= 0.5f * (1.0f - std::cos(6.2831853f * t));
    }
    ForwardFft(mSpectrumReal, mSpectrumImag);

    const size_t bins = fftSize / 2;
    if (mSpectrumDb.size() != bins)
      mSpectrumDb.assign(bins, -120.0f);

    const float scale = 2.0f / static_cast<float>(fftSize);
    for (size_t bin = 1; bin < bins; bin++)
    {
      const float magnitude =
        std::sqrt(mSpectrumReal[bin] * mSpectrumReal[bin] + mSpectrumImag[bin] * mSpectrumImag[bin]) * scale;
      const float db = 20.0f * std::log10(std::max(magnitude, 1.0e-7f));
      // Fast up, slow down - a peak hold that lets you see transients without the display
      // twitching on every frame.
      mSpectrumDb[bin] = (db > mSpectrumDb[bin]) ? db : mSpectrumDb[bin] + (db - mSpectrumDb[bin]) * 0.15f;
    }

    // Drawn as a filled shape under a line, one point per pixel column, taking the loudest bin
    // that falls in each column - at the low end many pixels share a bin, at the top many bins
    // share a pixel.
    const ImU32 fill = ImGui::GetColorU32(ImVec4(0.45f, 0.80f, 0.55f, 0.16f));
    const float binHz = static_cast<float>(sampleRate) / static_cast<float>(fftSize);
    const int columns = static_cast<int>(width);
    for (int column = 0; column < columns; column++)
    {
      const float hzLeft = hzForX(origin.x + static_cast<float>(column));
      const float hzRight = hzForX(origin.x + static_cast<float>(column + 1));
      const size_t firstBin = std::max<size_t>(1, static_cast<size_t>(hzLeft / binHz));
      const size_t lastBin = std::min(bins - 1, std::max(firstBin, static_cast<size_t>(hzRight / binHz)));

      float peak = -120.0f;
      for (size_t bin = firstBin; bin <= lastBin; bin++)
        peak = std::max(peak, mSpectrumDb[bin]);

      // -90..0 dBFS mapped over the plot height; anything quieter is not worth pixels.
      const float amount = std::clamp((peak + 90.0f) / 90.0f, 0.0f, 1.0f);
      if (amount <= 0.0f)
        continue;
      const float x = origin.x + static_cast<float>(column);
      draw->AddLine(ImVec2(x, plotMax.y), ImVec2(x, plotMax.y - height * amount), fill);
    }
  }

  // --- the response curve ---
  const int steps = std::max(32, static_cast<int>(width));
  ImVec2 previous;
  for (int i = 0; i <= steps; i++)
  {
    const float x = origin.x + width * static_cast<float>(i) / static_cast<float>(steps);
    const float db = ParametricEq::ResponseDb(settings, hzForX(x), sampleRate);
    const ImVec2 point(x, yForDb(db));
    if (i > 0)
      draw->AddLine(previous, point, ImGui::GetColorU32(theme::Control()), 2.0f);
    previous = point;
  }

  // --- handles ---
  bool changed = false;
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  // Picked up on press, released on let go. Holding the band index across frames is what lets the
  // pointer leave the handle mid-drag without the drag stopping.
  if (mDraggedEqBand >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    mDraggedEqBand = -1;

  if (plotHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
  {
    float nearest = 18.0f; // pixels
    for (size_t i = 0; i < kEqBandCount; i++)
    {
      const ImVec2 at(xForHz(settings.bands[i].hz), yForDb(settings.bands[i].gainDb));
      const float distance = std::sqrt((mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y));
      if (distance < nearest)
      {
        nearest = distance;
        mDraggedEqBand = static_cast<int>(i);
      }
    }

    // Pressing a point is also how its numbers are opened below the plot: the curve is the list of
    // bands, so there is no second list of them to keep in step with it.
    if (mDraggedEqBand >= 0)
      mSelectedEqBand = static_cast<size_t>(mDraggedEqBand);
  }

  if (mDraggedEqBand >= 0)
  {
    EqBand& band = settings.bands[static_cast<size_t>(mDraggedEqBand)];
    band.hz = std::clamp(hzForX(mouse.x), kBandMinHz, std::min(kBandMaxHz, nyquist));
    band.gainDb = std::clamp(dbForY(mouse.y), kBandMinGainDb, kBandMaxGainDb);
    changed = true;
  }

  // The wheel is the third dimension: Q has nowhere else to go on a two-axis plot.
  if (plotHovered && ImGui::GetIO().MouseWheel != 0.0f)
  {
    float nearest = 24.0f;
    int overBand = -1;
    for (size_t i = 0; i < kEqBandCount; i++)
    {
      const ImVec2 at(xForHz(settings.bands[i].hz), yForDb(settings.bands[i].gainDb));
      const float distance = std::sqrt((mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y));
      if (distance < nearest)
      {
        nearest = distance;
        overBand = static_cast<int>(i);
      }
    }
    if (overBand >= 0)
    {
      EqBand& band = settings.bands[static_cast<size_t>(overBand)];
      band.q = std::clamp(band.q * std::pow(1.2f, ImGui::GetIO().MouseWheel), kBandMinQ, kBandMaxQ);
      changed = true;
    }
  }

  for (size_t i = 0; i < kEqBandCount; i++)
  {
    const EqBand& band = settings.bands[i];
    const ImVec2 at(xForHz(band.hz), yForDb(band.gainDb));
    const bool active = (mDraggedEqBand == static_cast<int>(i));
    const bool open = (mSelectedEqBand == i);

    // Amber for the one being dragged, white for the rest - the same rule the handles everywhere
    // else in the app follow.
    const ImVec4 colour = !band.enabled ? theme::TextFaint() : (active ? theme::Accent() : theme::Control());
    draw->AddCircleFilled(at, active ? 8.0f : 6.0f, ImGui::GetColorU32(colour), 20);
    // A ring around the one whose numbers are open below, so the sliders and the curve cannot be
    // read as belonging to different points.
    if (open && !active)
      draw->AddCircle(at, 11.0f, ImGui::GetColorU32(theme::Accent()), 20, 1.8f);

    char number[4];
    std::snprintf(number, sizeof(number), "%d", static_cast<int>(i) + 1);
    const ImVec2 size = ImGui::CalcTextSize(number);
    draw->AddText(ImVec2(at.x - size.x * 0.5f, at.y - size.y - 10.0f), ImGui::GetColorU32(colour), number);
  }

  draw->PopClipRect();

  // Right under the pointer rather than in a corner: while dragging, this is the only place you
  // are looking.
  if (mDraggedEqBand >= 0)
  {
    const EqBand& band = settings.bands[static_cast<size_t>(mDraggedEqBand)];
    ImGui::SetTooltip("%.0f Hz   %+.1f dB   Q %.2f", band.hz, band.gainDb, band.q);
  }
  else if (plotHovered)
  {
    ImGui::SetTooltip("Drag a point to move it. Wheel over a point sets its Q");
  }

  return changed;
}

void MainWindow::DrawBlockPanel(int blockId)
{
  BlockSettings settings;
  if (!mEngine.GetBlockSettings(blockId, settings))
    return;

  const BlockSettings before = settings;
  bool settled = false;

  const auto blocks = mEngine.GetBlocks();

  // Only an EQ block has anywhere to show a spectrum, so only an EQ block asks the engine to tap
  // one. Set every frame the panel is open, and cleared as soon as it is not.
  mEngine.SetSpectrumBlock(settings.type == BlockType::Eq ? blockId : 0);

  // --- the header: what this block is, and what can be done to the whole of it ---
  //
  // A row, not a column. Whether the block is in the chain, what kind it is, and removing it were
  // a narrow strip down the left edge of the panel - which gave the most prominent place on the
  // screen to a decision made once per block, and parked Remove at the bottom of it, as far from
  // the block it removes as the panel allows.
  {
    const float powerSize = ImGui::GetFrameHeight() + 6.0f;
    settled |= theme::PowerButton("blockon", &settings.enabled, powerSize);

    ImGui::SameLine(0, 14);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
    theme::ViewTitle(BlockTypeName(settings.type));

    // Changing type throws away whatever was loaded, so each of these is the same operation with
    // a different destination.
    const auto typeButton = [&](const char* label, BlockType type)
    {
      ImGui::SameLine(0, 4);
      if (!SegmentedButton(label, settings.type == type, 66.0f) || settings.type == type)
        return;
      settings.type = type;
      mBlockPaths.erase(blockId);
      mBlockErrors.erase(blockId);
      mEngine.ClearBlockProcessor(blockId);
      settled = true;
    };

    ImGui::SameLine(0, 28);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    typeButton("NAM", BlockType::Nam);
    typeButton("IR", BlockType::Ir);
    typeButton("CUT", BlockType::Cut);
    typeButton("COMP", BlockType::Comp);
    typeButton("EQ", BlockType::Eq);

    // At the end of the line that names what it removes.
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 24.0f);
    if (theme::CloseButton("removeblock", 22.0f))
      mPendingRemoveBlockId = blockId;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Remove this block");

    theme::Divider();
  }

  // Three columns: how it is voiced, how loud, and what is loaded. Widths are shares of the window
  // rather than pixel counts, so the panel holds its shape at any size.
  const float width = ImGui::GetContentRegionAvail().x;
  const float height = ImGui::GetContentRegionAvail().y;
  const float gap = ImGui::GetStyle().ItemSpacing.x;

  // Not every block gets every control. A compressor carries its own gain and mix, the way the
  // pedal it is modelled on does, and an EQ block is nothing but levels of its own.
  const bool ownsLevels = (settings.type == BlockType::Comp || settings.type == BlockType::Eq);
  // The per-block tone stack is redundant on a block that is nothing but a tone stack, and out of
  // place on a cut: a low cut and a high cut are a tone control already, and having a second one
  // underneath them with its own bass and treble read as two EQs stacked in one box.
  const bool hasToneStack = (settings.type != BlockType::Eq && settings.type != BlockType::Cut);

  // --- everything the block is set to, as one list ---
  //
  // Gain, blend and volume were a column of knobs called LEVELS and the tone stack was another
  // section of knobs beside it - two clusters of circles that are the same kind of thing, drawn
  // twice. They are one list of rows now: name, slider, value. Nothing in a block is a knob.
  //
  // The right-hand column is only for a block that has something to show that is not a setting:
  // a library to pick from, or a curve. A compressor and a cut had one for a meter and a sentence,
  // which made two sections out of a block that is one list of values.
  const bool hasSource = (settings.type == BlockType::Nam || settings.type == BlockType::Ir
                          || settings.type == BlockType::Eq);
  const bool hasControls = (settings.type != BlockType::Eq);

  // The library is a list of names - it does not need the room a curve does - so it gets about a
  // third, and an EQ's plot gets the panel to itself.
  const float sourceWidth = !hasSource ? 0.0f : (hasControls ? std::clamp(width * 0.30f, 280.0f, 520.0f) : width);
  const float controlsWidth =
    !hasControls ? 0.0f : (hasSource ? std::max(320.0f, width - sourceWidth - gap) : width);

  const auto columnLabel = [](const char* text)
  {
    theme::PushHeading(0.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    theme::PopFont();
    ImGui::Spacing();
  };

  if (hasControls)
  {
    constexpr float kLabelColumn = 96.0f;
    ImGui::BeginChild("##colcontrols", ImVec2(controlsWidth, height), true);
    columnLabel("BLOCK");

    if (!ownsLevels)
    {
      // 100% is all of the block's own sound, which is what you want almost always.
      float blendPercent = (1.0f - settings.dryBlend) * 100.0f;
      if (BlockControl("blend", "BLEND", &blendPercent, 0.0f, 100.0f, 100.0f, "%.0f%%", kLabelColumn))
        settings.dryBlend = 1.0f - blendPercent / 100.0f;
      settled |= ImGui::IsItemDeactivatedAfterEdit();

      // Two separate things, deliberately: how hard the block is driven, and how loud it comes
      // out. Folding them into one control would hide the part that changes the tone.
      settled |= BlockControl("gain", "GAIN", &settings.gainDb, -24.0f, 24.0f, 0.0f, "%+.1f dB", kLabelColumn);
      settled |= BlockControl("volume", "VOLUME", &settings.levelDb, -24.0f, 24.0f, 0.0f, "%+.1f dB", kLabelColumn);
    }

    // --- what this particular type has, in the same list ---
    if (settings.type == BlockType::Comp)
    {
      settled |= BlockControl("peak", "PEAK", &settings.compPeak, 0.0f, 10.0f, 5.0f, "%.1f", kLabelColumn);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How hard it works. At 0 it leaves the signal alone");

      settled |= BlockControl("compgain", "MAKEUP", &settings.levelDb, -24.0f, 24.0f, 0.0f, "%+.1f dB", kLabelColumn);

      // --- what it is actually doing, in the row under what causes it ---
      //
      // A compressor you cannot see working is one you set by guesswork. It was a meter in a
      // second column with a paragraph beside it, which made two sections out of a block that is
      // one list; here it is the same shape as every row above it, and reads against them.
      {
        float reductionDb = 0.0f;
        for (size_t i = 0; i < blocks.size(); i++)
          if (blocks[i].id == blockId)
            reductionDb = mEngine.GetBlockGainReductionDb(i);

        theme::Label("REDUCTION");
        char reading[16];
        std::snprintf(reading, sizeof(reading), "-%.1f dB", reductionDb);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
                             - ImGui::CalcTextSize(reading).x);
        ImGui::PushStyleColor(ImGuiCol_Text, reductionDb > 0.05f ? theme::Text() : theme::TextFaint());
        ImGui::TextUnformatted(reading);
        ImGui::PopStyleColor();

        // Filling from the right, because what it shows is level being taken away. Scaled to 20 dB,
        // which is more reduction than this is meant to be pushed to.
        //
        // The empty track is drawn in the line colour, not the field colour: a field is white at
        // five percent, which on this black is invisible - so with nothing coming through, the
        // meter looked like it had been left out rather than like it was reading zero.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float meterWidth = ImGui::GetContentRegionAvail().x;
        constexpr float kMeterHeight = 8.0f;
        ImDrawList* meter = ImGui::GetWindowDrawList();
        meter->AddRectFilled(ImVec2(at.x, at.y + 5.0f), ImVec2(at.x + meterWidth, at.y + 5.0f + kMeterHeight),
                             ImGui::GetColorU32(theme::Line()), kMeterHeight * 0.5f);

        const float amount = std::clamp(reductionDb / 20.0f, 0.0f, 1.0f);
        if (amount > 0.0f)
          meter->AddRectFilled(ImVec2(at.x + meterWidth * (1.0f - amount), at.y + 5.0f),
                               ImVec2(at.x + meterWidth, at.y + 5.0f + kMeterHeight),
                               ImGui::GetColorU32(theme::Accent()), kMeterHeight * 0.5f);
        ImGui::Dummy(ImVec2(meterWidth, kMeterHeight + 12.0f));
      }

      float mixPercent = (1.0f - settings.dryBlend) * 100.0f;
      if (BlockControl("compmix", "MIX", &mixPercent, 0.0f, 100.0f, 100.0f, "%.0f%%", kLabelColumn))
        settings.dryBlend = 1.0f - mixPercent / 100.0f;
      settled |= ImGui::IsItemDeactivatedAfterEdit();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("100% is compressed only. Turn it down to blend the uncompressed signal back in");

      theme::LabelFor("MODE", kLabelColumn);
      if (SegmentedButton("COMP", !settings.compLimit, 78.0f) && settings.compLimit)
      {
        settings.compLimit = false;
        settled = true;
      }
      ImGui::SameLine(0, 4);
      if (SegmentedButton("LIMIT", settings.compLimit, 78.0f) && !settings.compLimit)
      {
        settings.compLimit = true;
        settled = true;
      }
    }
    else if (settings.type == BlockType::Cut)
    {
      // Each is off at the end it starts from, so it reads as "how much have I taken away" rather
      // than as a frequency you have to switch on first.
      char shown[24];
      const auto cutText = [&](float hz, bool off)
      {
        if (off)
          std::snprintf(shown, sizeof(shown), "Off");
        else if (hz >= 1000.0f)
          std::snprintf(shown, sizeof(shown), "%.2f kHz", hz / 1000.0f);
        else
          std::snprintf(shown, sizeof(shown), "%.0f Hz", hz);
        return shown;
      };

      settled |= BlockControl("lowcut", "LOW CUT", &settings.lowCutHz, kLowCutMinHz, kLowCutMaxHz, kLowCutMinHz,
                              "%.0f", kLabelColumn, cutText(settings.lowCutHz, settings.lowCutHz <= kLowCutMinHz));
      settled |= BlockControl("highcut", "HIGH CUT", &settings.highCutHz, kHighCutMinHz, kHighCutMaxHz,
                              kHighCutMaxHz, "%.0f", kLabelColumn,
                              cutText(settings.highCutHz, settings.highCutHz >= kHighCutMaxHz));
    }

    if (hasToneStack)
      DrawBlockEq(settings, settled, kLabelColumn);

    ImGui::EndChild();
    if (hasSource)
      ImGui::SameLine();
  }

  // --- what is loaded, or the curve ---
  //
  // Only for a block that has something to show which is not a setting. A compressor and a cut are
  // a list of values and nothing else, so they have no second column at all.
  if (hasSource)
  {
  ImGui::BeginChild("##colsource", ImVec2(sourceWidth, height), true);

  if (settings.type == BlockType::Eq)
  {
    columnLabel("EQUALISER");

    // --- one point at a time, beside the curve ---
    //
    // All five bands used to be laid out at once, fifteen drag fields under the plot, which did not
    // fit and so the panel scrolled. Press a point on the curve and its three values appear to the
    // right of it, standing on end: three faders fit in the height the plot already has, where
    // three rows underneath would push the last one off the bottom again.
    constexpr float kSliderColumn = 250.0f;
    const float plotWidth = std::max(240.0f, ImGui::GetContentRegionAvail().x - kSliderColumn - 16.0f);
    const float plotHeight = std::max(160.0f, ImGui::GetContentRegionAvail().y - 8.0f);

    if (DrawParametricEq(settings.peq, plotWidth, plotHeight))
      settled = true;

    ImGui::SameLine(0, 16);
    ImGui::BeginGroup();
    {
      const size_t band = std::min(mSelectedEqBand, kEqBandCount - 1);
      EqBand& open = settings.peq.bands[band];

      char heading[24];
      std::snprintf(heading, sizeof(heading), "POINT %d", static_cast<int>(band) + 1);

      // Switched in or out here rather than through a checkbox with a filled blue square in it -
      // the one control left in the app that was still ImGui's own colour.
      settled |= theme::PowerButton("bandon", &open.enabled, 22.0f);
      ImGui::SameLine(0, 10);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
      theme::Label(heading);

      // What is left of the plot's height after the name and value under each fader.
      const float faderHeight = std::max(80.0f, plotHeight - 76.0f);

      ImGui::BeginDisabled(!open.enabled);
      settled |= VerticalControl("bandhz", "FREQ", &open.hz, kBandMinHz, kBandMaxHz, 1000.0f, "%.0f", faderHeight);
      ImGui::SameLine(0, 10);
      settled |=
        VerticalControl("banddb", "GAIN", &open.gainDb, kBandMinGainDb, kBandMaxGainDb, 0.0f, "%+.1f", faderHeight);
      ImGui::SameLine(0, 10);
      settled |= VerticalControl("bandq", "Q", &open.q, kBandMinQ, kBandMaxQ, 1.0f, "%.2f", faderHeight);
      ImGui::EndDisabled();

      theme::Hint("Press a point on the curve to work on it");
    }
    ImGui::EndGroup();
  }
  else
  {
    columnLabel(settings.type == BlockType::Ir ? "IR" : "CAPTURE");
    const auto found = mBlockPaths.find(blockId);
    const bool loading = mCaptureLibrary.IsLoading(blockId);

    theme::PushHeading(1.15f);
    if (loading)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::TextUnformatted("loading...");
      ImGui::PopStyleColor();
    }
    else if (found == mBlockPaths.end() || found->second.empty())
    {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::TextUnformatted("Empty");
      ImGui::PopStyleColor();
    }
    else
    {
      ImGui::TextWrapped("%s", found->second.stem().string().c_str());
    }
    theme::PopFont();

    // --- what the engine is actually holding ---
    //
    // The name above comes from the UI's own record of what was asked for. Whether a processor
    // ever reached the block is a different fact, and until now nothing on this screen showed it -
    // so a load that quietly failed looked identical to one that worked, and the only symptom was
    // that the block did nothing to the sound.
    if (!loading && found != mBlockPaths.end() && !found->second.empty())
    {
      bool inChain = false;
      for (const auto& block : blocks)
        if (block.id == blockId)
          inChain = (settings.type == BlockType::Ir) ? (block.ir != nullptr) : (block.namModel != nullptr);

      if (!inChain)
      {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
        ImGui::TextWrapped("Not in the chain - the file is remembered but nothing is loaded into "
                           "this block, so it passes the signal straight through.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Load it again", ImVec2(-1, 0)))
          LoadFileIntoBlock(blockId, found->second);
      }
    }

    ImGui::Spacing();

    // The library itself, cut down to what fits: picking the next capture is the thing you do most
    // in this panel, and leaving the whole view to do it loses your place in the chain.
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##blockpicker", "Search library", mBlockPickerText, sizeof(mBlockPickerText));

    const std::string needle = ToLower(std::string(mBlockPickerText));
    const bool wantIr = (settings.type == BlockType::Ir);
    const std::filesystem::path loadedPath =
      (found != mBlockPaths.end()) ? found->second : std::filesystem::path();

    // Two buttons and their spacing are what has to stay visible below the list.
    const float reserved = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
    const float pickerHeight = std::max(80.0f, ImGui::GetContentRegionAvail().y - reserved);

    // --- two lists: the folders, and what is in the one you picked ---
    //
    // It was one flat list with the folder repeated down the left of every row - forty rows all
    // saying "Ampeg SVT Classic with 6x10" beside forty different mic positions. Choosing which
    // amp and choosing which capture of it are two steps, so they are two lists.
    const float folderWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 120.0f, 240.0f);

    // Collected fresh each frame from the entries the search leaves, so a search narrows the
    // folder list too rather than leaving folders that turn out to be empty.
    std::vector<const std::string*> folders;
    for (const auto& entry : mCaptureLibrary.GetEntries())
    {
      if (entry.isIr != wantIr)
        continue;
      if (!needle.empty() && entry.searchHaystack.find(needle) == std::string::npos)
        continue;

      bool seen = false;
      for (const std::string* known : folders)
        seen = seen || (*known == entry.groupName);
      if (!seen)
        folders.push_back(&entry.groupName);
    }
    std::sort(folders.begin(), folders.end(),
              [](const std::string* a, const std::string* b) { return CompareNoCase(*a, *b) < 0; });

    // The folder the loaded file is in, when nothing has been picked by hand yet: opening the
    // panel should show you where you already are.
    if (mBlockFolder.empty() && !loadedPath.empty())
      for (const auto& entry : mCaptureLibrary.GetEntries())
        if (entry.path == loadedPath)
          mBlockFolder = entry.groupName;

    bool folderExists = false;
    for (const std::string* known : folders)
      folderExists = folderExists || (*known == mBlockFolder);
    if (!folderExists)
      mBlockFolder = folders.empty() ? std::string() : *folders.front();

    ImGui::BeginChild("##blockfolders", ImVec2(folderWidth, pickerHeight), true);
    for (const std::string* known : folders)
    {
      const std::string& name = *known;
      if (ImGui::Selectable(name.empty() ? "(loose files)" : name.c_str(), name == mBlockFolder))
        mBlockFolder = name;
    }
    if (folders.empty())
      theme::Hint(needle.empty() ? "Library is empty" : "Nothing matches");
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##blockfiles", ImVec2(0, pickerHeight), true);
    {
      int shown = 0;
      for (const auto& entry : mCaptureLibrary.GetEntries())
      {
        if (entry.isIr != wantIr || entry.groupName != mBlockFolder)
          continue;
        if (!needle.empty() && entry.searchHaystack.find(needle) == std::string::npos)
          continue;

        ImGui::PushID(shown++);
        if (ImGui::Selectable(entry.displayName.c_str(), entry.path == loadedPath))
          LoadFileIntoBlock(blockId, entry.path);
        ImGui::PopID();
      }
      if (shown == 0)
        theme::Hint("Nothing in this folder");
    }
    ImGui::EndChild();

    if (ImGui::Button("File...", ImVec2(-1, 0)))
    {
      HWND hwnd = glfwGetWin32Window(mWindow);
      const bool isIr = (settings.type == BlockType::Ir);
      auto picked = BrowseForFile(hwnd, isIr ? L"Choose a cab impulse response" : L"Choose a NAM capture",
                                  isIr ? L"WAV audio" : L"NAM capture", isIr ? L"wav" : L"nam");
      if (picked.has_value())
        LoadFileIntoBlock(blockId, *picked);
    }
    ImGui::BeginDisabled(found == mBlockPaths.end());
    if (ImGui::Button("Clear", ImVec2(-1, 0)))
    {
      mEngine.ClearBlockProcessor(blockId);
      mBlockPaths.erase(blockId);
      mBlockErrors.erase(blockId);
      SaveChainToConfig();
    }
    ImGui::EndDisabled();

    const auto error = mBlockErrors.find(blockId);
    if (error != mBlockErrors.end())
    {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
      ImGui::TextWrapped("%s", error->second.c_str());
      ImGui::PopStyleColor();
    }

    // A capture trained at another rate will not sound as intended.
    for (const auto& block : blocks)
    {
      if (block.id != blockId || !block.namModel)
        continue;
      const double expected = block.namModel->GetExpectedSampleRate();
      const unsigned int actual = mEngine.GetActualSampleRate();
      if (expected > 0 && actual > 0 && std::fabs(expected - static_cast<double>(actual)) > 0.5)
      {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
        ImGui::Text("Wants %.0f Hz, running %u Hz", expected, actual);
        ImGui::PopStyleColor();
      }
    }
  }
  ImGui::EndChild();
  }

  // One write covers every control above; the engine only sees whole, consistent settings.
  if (settings.enabled != before.enabled || settings.type != before.type || settings.gainDb != before.gainDb
      || settings.levelDb != before.levelDb || settings.dryBlend != before.dryBlend || settings.row != before.row
      || settings.eq != before.eq || settings.lowCutHz != before.lowCutHz || settings.highCutHz != before.highCutHz
      || settings.compPeak != before.compPeak || settings.compLimit != before.compLimit
      || settings.peq != before.peq)
    mEngine.SetBlockSettings(blockId, settings);
  if (settled)
    SaveChainToConfig();
}
/// \brief The block's tone stack, as four more faders in the same row as everything else.
///
/// It used to be a section of its own with a heading, a switch and four knobs, sitting beside a
/// second section of three knobs called LEVELS - two clusters of circles that were the same kind
/// of thing drawn twice. A block is a set of values; they belong in one row.
///
/// \param faderHeight so it matches the levels beside it exactly
void MainWindow::DrawBlockEq(BlockSettings& settings, bool& settled, float labelWidth)
{
  EqSettings& eq = settings.eq;

  // Whether the stack is in, and where it acts, on one line above its four values. Pre or post
  // matters: before the block it changes how hard each band drives whatever is loaded, and
  // therefore how it distorts; after, it only shapes the result.
  ImGui::Dummy(ImVec2(0.0f, 4.0f));
  settled |= theme::PowerButton("eqon", &eq.enabled, 24.0f);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(eq.enabled ? "Tone stack in" : "Tone stack out");

  ImGui::SameLine(0, 10);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
  theme::Label("TONE");

  ImGui::SameLine(0, 16);
  ImGui::BeginDisabled(!eq.enabled);
  const bool isPre = (eq.placement == EqPlacement::Pre);
  if (SegmentedButton("PRE", isPre, 58.0f) && !isPre)
  {
    eq.placement = EqPlacement::Pre;
    settled = true;
  }
  ImGui::SameLine(0, 4);
  if (SegmentedButton("POST", !isPre, 58.0f) && isPre)
  {
    eq.placement = EqPlacement::Post;
    settled = true;
  }

  settled |= BlockControl("low", "LOW", &eq.lowDb, kEqMinGainDb, kEqMaxGainDb, 0.0f, "%+.1f dB", labelWidth);
  settled |= BlockControl("mid", "MID", &eq.midDb, kEqMinGainDb, kEqMaxGainDb, 0.0f, "%+.1f dB", labelWidth);
  settled |= BlockControl("high", "HIGH", &eq.highDb, kEqMinGainDb, kEqMaxGainDb, 0.0f, "%+.1f dB", labelWidth);

  // The mid is swept rather than fixed: where the mids sit is completely different on bass than on
  // guitar, and it is the band worth moving.
  settled |= BlockControl("midhz", "MID FREQ", &eq.midHz, kEqMinMidHz, kEqMaxMidHz, 700.0f, "%.0f Hz", labelWidth);

  ImGui::EndDisabled();
}

void MainWindow::UpdateTunerReference()
{
  mEngine.GetTuner().SetReferenceHz(
    NoteFrequency(mTunerNoteIndex, mTunerOctave, static_cast<double>(mTunerA4Hz)));
}

void MainWindow::SetTunerAuto(bool automatic)
{
  mTunerAuto = automatic;
  mTunerCandidateCount = 0;
  mConfig.tunerAuto = automatic;
  mConfig.Save();
}

/// \brief Follows what is being played and points the strobe at the nearest note.
///
/// Nearest string of the chosen tuning when there is one, rather than nearest semitone. A slack
/// low E reads as F on a chromatic tuner on the way up, and every semitone it passes takes the
/// display with it; snapping to the strings means the note you are winding towards is the note
/// on the screen the whole time.
void MainWindow::FollowDetectedNote()
{
  if (!mTunerAuto)
  {
    mTunerCandidateCount = 0;
    return;
  }

  const double hz = mEngine.GetTuner().GetDetectedHz();
  if (hz <= 0.0)
  {
    mTunerCandidateCount = 0;
    return;
  }

  const double a4 = static_cast<double>(mTunerA4Hz);
  int note = mTunerNoteIndex;
  int octave = mTunerOctave;

  const TuningPreset* active = FindTuning(mTunerInstrument, mTunerStringCount, mTunerTuningName);
  if (active != nullptr)
  {
    TunedString strings[8];
    const int count = EffectiveStrings(*active, mTunerCustomStrings, mTunerStringMidi.data(), strings);

    double closest = 1.0e9;
    for (int i = 0; i < count; i++)
    {
      const TunedString& string = strings[i];
      // Compared in cents, not hertz: the strings are spaced evenly in pitch, and in hertz the
      // gap between the top two is several times the gap between the bottom two.
      const double distance = std::fabs(1200.0 * std::log2(hz / NoteFrequency(string.noteIndex, string.octave, a4)));
      if (distance < closest)
      {
        closest = distance;
        note = string.noteIndex;
        octave = string.octave;
      }
    }
    // Further than three semitones from every string is another instrument, a harmonic, or a
    // fretted note - not the string being tuned.
    if (closest > 300.0)
    {
      mTunerCandidateCount = 0;
      return;
    }
  }
  else
  {
    const double midi = 69.0 + 12.0 * std::log2(hz / a4);
    const int rounded = static_cast<int>(std::lround(midi));
    note = ((rounded % 12) + 12) % 12;
    octave = rounded / 12 - 1;
  }

  if (note == mTunerNoteIndex && octave == mTunerOctave)
  {
    mTunerCandidateCount = 0;
    return;
  }

  if (note == mTunerCandidateNote && octave == mTunerCandidateOctave)
    mTunerCandidateCount++;
  else
  {
    mTunerCandidateNote = note;
    mTunerCandidateOctave = octave;
    mTunerCandidateCount = 1;
  }

  // Three readings at twenty a second: a seventh of a second of agreement before the display moves.
  if (mTunerCandidateCount >= 3)
  {
    mTunerNoteIndex = note;
    mTunerOctave = octave;
    mTunerCandidateCount = 0;
    // Not saved to disk: what auto lands on is a reading, not a setting.
  }
}


namespace
{
/// m:ss.t - tenths, because a loop point is often a beat and a beat is a fraction of a second.
std::string FormatTime(double seconds)
{
  if (seconds < 0.0)
    seconds = 0.0;
  const int minutes = static_cast<int>(seconds) / 60;
  const int wholeSeconds = static_cast<int>(seconds) % 60;
  const int tenths = static_cast<int>((seconds - std::floor(seconds)) * 10.0);

  char text[32];
  std::snprintf(text, sizeof(text), "%d:%02d.%d", minutes, wholeSeconds, tenths);
  return text;
}
} // namespace

int MainWindow::AddPlayerTrackFromPath(const std::filesystem::path& path)
{
  // Decoded on this thread. A five minute file takes a moment, and the alternative - a loader
  // thread with progress - is worth having only once tracks are opened often enough to notice.
  auto file = std::make_shared<AudioFile>();
  const double rate = std::max(8000.0, static_cast<double>(mEngine.GetActualSampleRate()));
  if (!AudioFile::Load(path, rate, *file, mPlayerError))
    return -1;

  auto peaks = std::make_shared<PeakCache>();
  peaks->Build(*file);

  mPlayerError.clear();
  mProjectDirty = true; // a track that took a moment to decode is worth being asked about
  return mEngine.GetPlayer().AddTrack(std::move(file), std::move(peaks));
}

void MainWindow::AddPlayerTrack()
{
  HWND hwnd = glfwGetWin32Window(mWindow);
  auto picked = BrowseForFile(hwnd, L"Open an audio file", L"Audio files", L"wav;*.mp3;*.flac");
  if (picked.has_value())
    AddPlayerTrackFromPath(*picked);
}

double MainWindow::SnapToBeat(double seconds) const
{
  if (!mSnapToGrid || !mTempo.valid || mTempo.bpm <= 0.0f)
    return seconds;

  // Whether the grid was tracked through the song or typed in as one number, this is the same
  // call - the estimate knows which it is holding.
  return mTempo.NearestBeat(seconds);
}

float MainWindow::LaneHeight(size_t trackCount, float available) const
{
  const size_t count = std::max<size_t>(1, trackCount);
  if (mLaneHeight > 0.0f)
    return mLaneHeight;
  return std::max(24.0f, available / static_cast<float>(count));
}

std::vector<double> MainWindow::BeatTimes()
{
  if (!mTempo.valid || mTempo.bpm <= 0.0f)
    return {};

  // The tracked beats when there are any: chords change on the song's beats, not on a metronome's.
  if (!mTempo.beats.empty())
    return mTempo.beats;

  std::vector<double> beats;
  const double beat = 60.0 / static_cast<double>(mTempo.bpm);
  const double duration = mEngine.GetPlayer().DurationSeconds();
  for (double t = mTempo.firstBeatSeconds; t < duration + beat; t += beat)
    if (t >= 0.0)
      beats.push_back(t);
  return beats;
}

void MainWindow::RebuildSynthScore()
{
  const double rate = std::max(8000.0, static_cast<double>(mEngine.GetActualSampleRate()));

  std::vector<SynthNote> notes;
  std::vector<SynthPart> parts;

  for (auto& entry : mTrackNotes)
  {
    NoteTrack& track = entry.second;
    if (!track.playNotes && !track.playChords)
      continue;

    const int part = static_cast<int>(parts.size());
    SynthPart settings;
    settings.instrument = static_cast<SynthInstrument>(
      std::clamp(track.instrument, 0, static_cast<int>(SynthInstrument::Count) - 1));
    settings.gainDb = track.playGainDb;
    parts.push_back(settings);

    // Applied here rather than to what was found, so moving it never touches the analysis - the
    // lane goes on drawing the notes that are actually in the recording.
    const int shift = std::clamp(track.playOctave, -3, 3) * 12;

    if (track.playNotes)
    {
      for (const auto& note : track.notes)
      {
        SynthNote out;
        out.startSample = static_cast<long long>(note.startSeconds * rate);
        out.endSample = static_cast<long long>(note.endSeconds * rate);
        out.midi = std::clamp(note.midi + shift, 0, 127);
        // How sure the detector was becomes how hard the note is struck, so a guess sounds like
        // one rather than sitting on top of the music.
        out.velocity = std::clamp(0.45f + 0.55f * note.confidence, 0.1f, 1.0f);
        out.part = part;
        notes.push_back(out);
      }
    }

    if (track.playChords)
    {
      for (const auto& chord : track.chords)
      {
        // Voiced from the octave below middle C upwards, the way somebody would play it with one
        // hand. The root also goes down an octave so the chord has a bottom to it.
        const std::vector<int> intervals = ChordIntervals(chord.quality);
        const int base = 48 + chord.root; // C3 upwards

        for (size_t i = 0; i <= intervals.size(); i++)
        {
          SynthNote out;
          out.startSample = static_cast<long long>(chord.startSeconds * rate);
          out.endSample = static_cast<long long>(chord.endSeconds * rate);
          out.midi = std::clamp(((i == 0) ? base - 12 : base + intervals[i - 1]) + shift, 0, 127);
          out.velocity = (i == 0) ? 0.75f : 0.55f;
          out.part = part;
          notes.push_back(out);
        }
      }
    }
  }

  // One cursor walks the lot on the audio thread, so it has to be in time order.
  std::sort(notes.begin(), notes.end(),
            [](const SynthNote& a, const SynthNote& b) { return a.startSample < b.startSample; });

  mEngine.GetPlayer().GetSynth().SetScore(std::move(notes), std::move(parts));
}

bool MainWindow::DrawPlaybackControls(NoteTrack& notes)
{
  bool changed = false;

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextUnformatted("PLAY BACK");
  ImGui::PopStyleColor();

  if (ImGui::MenuItem("Play the notes", nullptr, notes.playNotes, !notes.notes.empty()))
  {
    notes.playNotes = !notes.playNotes;
    changed = true;
  }
  if (ImGui::MenuItem("Play the chords", nullptr, notes.playChords, !notes.chords.empty()))
  {
    notes.playChords = !notes.playChords;
    changed = true;
  }

  if (ImGui::BeginMenu("Instrument"))
  {
    for (int i = 0; i < static_cast<int>(SynthInstrument::Count); i++)
    {
      const auto instrument = static_cast<SynthInstrument>(i);
      if (ImGui::MenuItem(SynthInstrumentName(instrument), nullptr, notes.instrument == i))
      {
        notes.instrument = i;
        changed = true;
      }
    }
    ImGui::EndMenu();
  }

  // Which octave it comes out in. A bass line played back at written pitch sits in the same
  // register as the bass it was found in and the two fight; moved up, it sits above the music
  // where it can be heard against it. Only the playback moves - the lane still draws what was
  // actually found.
  if (ImGui::BeginMenu("Octave"))
  {
    for (int octave = 3; octave >= -3; octave--)
    {
      char label[16];
      if (octave == 0)
        std::snprintf(label, sizeof(label), "As found");
      else
        std::snprintf(label, sizeof(label), "%+d", octave);

      if (ImGui::MenuItem(label, nullptr, notes.playOctave == octave))
      {
        notes.playOctave = octave;
        changed = true;
      }
    }
    ImGui::EndMenu();
  }

  ImGui::SetNextItemWidth(160);
  if (ImGui::SliderFloat("Level", &notes.playGainDb, -30.0f, 6.0f, "%+.0f dB"))
    changed = true;

  return changed;
}

void MainWindow::DrawNoteLane(const NoteTrack& notes, float top, float height, float left, float right,
                              double viewStart, double viewSpan, bool hovered)
{
  if ((notes.notes.empty() && notes.chords.empty()) || height < 6.0f)
    return;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float paneWidth = right - left;

  draw->AddLine(ImVec2(left, top), ImVec2(right, top), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)));

  const auto xFor = [&](double seconds)
  { return left + paneWidth * static_cast<float>(std::clamp((seconds - viewStart) / viewSpan, 0.0, 1.0)); };

  // --- chords across the top ---
  //
  // The thing a guitarist actually reads. Given the whole width of the lane and its own row above
  // the notes, because "Em for two bars" is the answer to the question, and the notes underneath
  // are the working.
  const float chordHeight = notes.chords.empty() ? 0.0f : std::min(26.0f, height);
  if (!notes.chords.empty())
  {
    const ImU32 chordFill = ImGui::GetColorU32(ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.22f));
    const ImU32 chordEdge = ImGui::GetColorU32(theme::Accent());

    for (const auto& chord : notes.chords)
    {
      if (chord.endSeconds < viewStart || chord.startSeconds > viewStart + viewSpan)
        continue;

      const float x0 = xFor(chord.startSeconds);
      const float x1 = xFor(chord.endSeconds);
      const ImVec2 from(x0, top + 1.0f);
      const ImVec2 to(std::max(x1 - 1.0f, x0 + 2.0f), top + chordHeight - 1.0f);

      draw->AddRectFilled(from, to, chordFill, 3.0f);
      draw->AddLine(ImVec2(x0, from.y), ImVec2(x0, to.y), chordEdge, 1.5f);

      const std::string name = ChordName(chord);
      const ImVec2 size = ImGui::CalcTextSize(name.c_str());
      if (to.x - from.x >= size.x + 8.0f && chordHeight >= size.y)
        draw->AddText(ImVec2(from.x + 5.0f, from.y + (chordHeight - 2.0f - size.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_Text), name.c_str());

      if (hovered && ImGui::GetIO().MousePos.x >= from.x && ImGui::GetIO().MousePos.x <= to.x
          && ImGui::GetIO().MousePos.y >= from.y && ImGui::GetIO().MousePos.y <= to.y)
      {
        ImGui::SetTooltip("%s   %.1f s   %.0f%% sure\nClick to hear it", name.c_str(), chord.DurationSeconds(),
                          std::clamp(chord.confidence / 1.25f, 0.0f, 1.0f) * 100.0f);

        // A chord name is a claim about the music, and the only way to judge one is to hear it
        // against what it was found in. Voiced the same way the playback does, so pressing it and
        // playing the track through give the same answer.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
          mNoteAudition = true; // the click was for this, not for the playhead
          const std::vector<int> intervals = ChordIntervals(chord.quality);
          const int base = 48 + chord.root;

          // The same octave the playback uses, so pressing it and hearing it play through the song
          // are the same sound.
          const int shift = std::clamp(notes.playOctave, -3, 3) * 12;

          std::vector<int> midi;
          midi.push_back(std::clamp(base - 12 + shift, 0, 127));
          for (const int interval : intervals)
            midi.push_back(std::clamp(base + interval + shift, 0, 127));

          mEngine.GetPlayer().GetSynth().Audition(midi.data(), static_cast<int>(midi.size()),
                                                  static_cast<SynthInstrument>(std::clamp(
                                                    notes.instrument, 0, static_cast<int>(SynthInstrument::Count) - 1)),
                                                  notes.playGainDb, 1.4f);
        }
      }
    }
  }

  top += chordHeight;
  height -= chordHeight;

  if (notes.notes.empty() || height < 6.0f)
    return;

  // --- two shapes for two kinds of answer ---
  //
  // One note at a time is a line, and a line reads best as fat blocks placed by pitch: the contour
  // is visible and there is room for a name in each. Several notes at once is a chord, and a chord
  // only makes sense as a piano roll - a row per semitone, thin, notes stacked where they are
  // played together.
  const int span = std::max(1, notes.highestMidi - notes.lowestMidi);
  const float rowHeight = notes.polyphonic ? height / static_cast<float>(span + 1) : 0.0f;

  const float blockHeight = notes.polyphonic ? std::max(2.0f, std::min(rowHeight - 1.0f, 16.0f))
                                             : std::min(18.0f, std::max(4.0f, height - 4.0f));
  const float travel = std::max(0.0f, height - blockHeight - 3.0f);
  const float fontHeight = ImGui::GetTextLineHeight();
  const bool roomForNames = blockHeight >= fontHeight - 1.0f;

  bool describedOne = false;
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  for (const auto& note : notes.notes)
  {
    if (note.endSeconds < viewStart || note.startSeconds > viewStart + viewSpan)
      continue;

    const float x0 =
      left + paneWidth * static_cast<float>(std::clamp((note.startSeconds - viewStart) / viewSpan, 0.0, 1.0));
    const float x1 =
      left + paneWidth * static_cast<float>(std::clamp((note.endSeconds - viewStart) / viewSpan, 0.0, 1.0));

    const float y = notes.polyphonic
                      ? top + static_cast<float>(notes.highestMidi - note.midi) * rowHeight
                      : top + 2.0f
                          + (1.0f - static_cast<float>(note.midi - notes.lowestMidi) / static_cast<float>(span))
                              * travel;
    const ImVec2 from(x0, y);
    const ImVec2 to(std::max(x1, x0 + 2.0f), y + blockHeight);

    // Hue by pitch class, so the same note is the same colour every time it comes round and a
    // repeated figure is visible as a pattern before you have read a single name. How solid it is
    // says how sure the detector was, so a guess looks like one.
    // White, at a strength that says how sure the detector was. The pitch is written on the block
    // and drawn by where it sits; giving each of the twelve its own hue said the same thing a
    // third time, in colour the rest of the screen has stopped using.
    const float alpha = 0.30f + 0.55f * std::clamp(note.confidence, 0.0f, 1.0f);
    const ImU32 fill = ImGui::GetColorU32(ImVec4(1, 1, 1, alpha));

    draw->AddRectFilled(from, to, fill, 2.0f);

    if (roomForNames)
    {
      const std::string name = MidiNoteName(note.midi);
      const ImVec2 size = ImGui::CalcTextSize(name.c_str());
      if (to.x - from.x >= size.x + 7.0f)
        draw->AddText(ImVec2(from.x + 4.0f, from.y + (blockHeight - size.y) * 0.5f),
                      ImGui::GetColorU32(ImVec4(0.05f, 0.06f, 0.08f, 0.95f)), name.c_str());
    }

    // Everything the block cannot fit - how far off the pitch was, how sure the detector is - is
    // one hover away rather than crowding the lane.
    if (hovered && !describedOne && mouse.x >= from.x && mouse.x <= to.x && mouse.y >= from.y && mouse.y <= to.y)
    {
      describedOne = true;
      ImGui::SetTooltip("%s  %+.0f cents\n%.2f s   %.0f%% sure\nClick to hear it",
                        MidiNoteName(note.midi).c_str(), note.centsOffset, note.DurationSeconds(),
                        note.confidence * 100.0f);

      // Sounded for as long as it was found to last, up to a point - a note held for eight bars
      // does not need eight bars of piano to tell you what it is.
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      {
        mNoteAudition = true; // the click was for this, not for the playhead
        const int midi = std::clamp(note.midi + std::clamp(notes.playOctave, -3, 3) * 12, 0, 127);
        mEngine.GetPlayer().GetSynth().Audition(
          &midi, 1,
          static_cast<SynthInstrument>(
            std::clamp(notes.instrument, 0, static_cast<int>(SynthInstrument::Count) - 1)),
          notes.playGainDb, std::clamp(static_cast<float>(note.DurationSeconds()), 0.25f, 2.0f));
      }
    }
  }
}

void MainWindow::DrawTimeline(float width, float height, float laneHeight)
{
  Player& player = mEngine.GetPlayer();
  const auto tracks = player.GetTracks();
  auto loops = player.GetLoops();
  const double duration = player.DurationSeconds();

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // Without this the timeline claims the pointer for the whole of its area and keeps it: ImGui
  // gives hover to the first item that asks, so anything placed over the top afterwards - a button
  // in a lane, say - is drawn but can never be pressed.
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##timeline", ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 area = ImVec2(origin.x + width, origin.y + height);
  // No fill behind the whole timeline: the lanes each bring their own, and the ruler its own, so
  // anything under them is a floor that lifts every shade above it.
  draw->PushClipRect(origin, area, true);

  // --- the visible window ---
  //
  // Everything below works in view coordinates rather than whole-file ones, so zooming is a
  // change to two numbers instead of a special case in each part that draws.
  if (mViewDuration <= 0.0 || mViewDuration > duration)
    mViewDuration = duration;
  mViewStart = std::clamp(mViewStart, 0.0, std::max(0.0, duration - mViewDuration));

  // Keep the playhead on screen while it is moving, but only when zoomed in - at full zoom there
  // is nowhere for it to go.
  if (mFollowPlayhead && player.IsPlaying() && mViewDuration < duration)
  {
    const double position = player.GetPositionSeconds();
    if (position < mViewStart || position > mViewStart + mViewDuration)
      mViewStart = std::clamp(position - mViewDuration * 0.5, 0.0, std::max(0.0, duration - mViewDuration));
  }

  const double viewStart = mViewStart;
  const double viewSpan = std::max(1.0e-6, mViewDuration);

  const auto xForTime = [&](double seconds)
  { return origin.x + width * static_cast<float>(std::clamp((seconds - viewStart) / viewSpan, 0.0, 1.0)); };
  const auto timeForX = [&](float x)
  { return viewStart + viewSpan * static_cast<double>(std::clamp((x - origin.x) / width, 0.0f, 1.0f)); };

  // The ruler takes the top strip and stays put; the lanes scroll underneath it, because bar
  // numbers are the one thing you always need in view. Both this and the track controls on the
  // left work from the same lane height and the same scroll, so a lane and its controls stay level.
  const float lanesTop = origin.y + kRulerHeight;
  const float lanesHeight = std::max(1.0f, height - kRulerHeight);

  // Set by a note or a chord that took this frame's click, so the seek further down leaves it be.
  mNoteAudition = false;

  // Which beats begin a bar. When the model marked them, those are used: a bar count worked out
  // from one offset and a fixed length is only right while the meter never changes, and a single
  // dropped beat puts every bar line after it on the wrong one.
  const int beatsPerBar = std::max(1, mGridBeatsPerBar);

  // Only when there is a flag for every beat. A half-filled list would have the ruler counting the
  // model's bars up to the last beat it knew about and then repeating that number for the rest of
  // the song, which is what it did.
  const bool useFlags = !mTempo.downbeats.empty() && mTempo.downbeats.size() == mTempo.beats.size();

  // Counted once for the frame rather than once per bar line. The ruler asks for a bar number at
  // every beat in view, and counting flags from the start each time is a loop inside a loop.
  std::vector<int> barsUpTo;
  if (useFlags)
  {
    barsUpTo.resize(mTempo.downbeats.size());
    int count = 0;
    for (size_t i = 0; i < mTempo.downbeats.size(); i++)
    {
      count += (mTempo.downbeats[i] != 0) ? 1 : 0;
      barsUpTo[i] = count;
    }
  }

  const auto isBarLine = [&](long long index)
  {
    if (useFlags && index >= 0 && static_cast<size_t>(index) < mTempo.downbeats.size())
      return mTempo.downbeats[static_cast<size_t>(index)] != 0;
    return (((index - mTempo.downbeatOffset) % beatsPerBar) + beatsPerBar) % beatsPerBar == 0;
  };

  const auto barNumberOf = [&](long long index) -> long long
  {
    if (!barsUpTo.empty() && index >= 0)
    {
      // Counted rather than divided, because the bars are not all the same length once the model
      // has had its say.
      if (static_cast<size_t>(index) < barsUpTo.size())
        return std::max(1, barsUpTo[static_cast<size_t>(index)]);

      // Past the last beat it gave us - the song runs on beyond what was analysed - so the count
      // carries on at the length the piece was mostly in.
      const long long beyond = index - static_cast<long long>(barsUpTo.size()) + 1;
      return barsUpTo.back() + beyond / beatsPerBar;
    }

    const long long fromDownbeat = index - mTempo.downbeatOffset;
    const long long bar = (fromDownbeat >= 0) ? fromDownbeat / beatsPerBar
                                              : -((-fromDownbeat + beatsPerBar - 1) / beatsPerBar);
    return bar + 1; // bar 1 is the first, not bar 0
  };

  // --- one waveform per track, stacked ---
  if (!tracks.empty())
  {
    // The lanes are clipped to their own strip so a scrolled-up lane cannot draw over the ruler.
    draw->PushClipRect(ImVec2(origin.x, lanesTop), ImVec2(area.x, area.y), true);

    // --- the beat grid, down the whole stack and behind everything on it ---
    //
    // The ruler has always numbered the bars, which answers "which bar is this" but not "where in
    // this bar does that note fall" - for that the line has to come down past the waveform it is
    // being read against. Drawn first, so it lies under the waveforms rather than over them:
    // a grid you read through, not one you read instead.
    if (mShowGrid && mTempo.valid && mTempo.bpm > 0.0f)
    {
      const std::vector<double> gridBeats = BeatTimes();
      const double perPixel = viewSpan / static_cast<double>(std::max(1.0f, width));

      // Below about six pixels apart a line per beat is a wash rather than a grid, so at that point
      // only the bars are drawn - and below the same spacing for bars, nothing is.
      const double beatSpacing = (gridBeats.size() > 1) ? (gridBeats[1] - gridBeats[0]) / perPixel : 1.0e9;
      const bool everyBeat = beatSpacing > 6.0;
      const bool anyBars = beatSpacing * std::max(1, mGridBeatsPerBar) > 6.0;

      const ImU32 beatLine = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f));
      const ImU32 barLine = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.16f));

      if (anyBars)
      {
        for (size_t i = 0; i < gridBeats.size(); i++)
        {
          if (gridBeats[i] < viewStart || gridBeats[i] > viewStart + viewSpan)
            continue;

          const bool bar = isBarLine(static_cast<long long>(i));
          if (!bar && !everyBeat)
            continue;

          const float gx = xForTime(gridBeats[i]);
          draw->AddLine(ImVec2(gx, lanesTop), ImVec2(gx, area.y), bar ? barLine : beatLine, bar ? 1.4f : 1.0f);
        }
      }
    }

    for (size_t t = 0; t < tracks.size(); t++)
    {
      const Track& track = tracks[t];
      const float top = lanesTop - mLaneScroll + laneHeight * static_cast<float>(t);

      // Nothing to draw for a lane that is scrolled out of sight, and a long song's worth of
      // waveform columns is not cheap to work out.
      if (top + laneHeight < lanesTop || top > area.y)
        continue;

      // Whether this track is going to be heard, which decides how strongly the whole lane is
      // drawn. Worked out before anything is put down, because the slab behind the waveform
      // dims with it.
      bool anySoloed = false;
      for (const auto& other : tracks)
        if (other.soloed)
          anySoloed = true;
      const bool audibleLane = !track.muted && (!anySoloed || track.soloed);

      // The notes take the bottom of the lane and the waveform keeps the rest, so the two are
      // read together: the shape you can hear, and the name of what made it.
      const auto found = mTrackNotes.find(track.id);
      const bool showNotes = found != mTrackNotes.end() && found->second.visible
                             && (!found->second.notes.empty() || !found->second.chords.empty());

      // Chords get a fixed row; notes get what they need under it - a chord stack more than a
      // single line does.
      const float chordRow = (showNotes && !found->second.chords.empty()) ? 26.0f : 0.0f;
      const float noteRows = (!showNotes || found->second.notes.empty()) ? 0.0f
                             : found->second.polyphonic ? std::min(110.0f, laneHeight * 0.5f)
                                                        : std::min(34.0f, laneHeight * 0.36f);
      const float noteHeight = std::min(chordRow + noteRows, laneHeight * 0.75f);
      const float waveHeight = laneHeight - noteHeight;

      const float centre = top + waveHeight * 0.5f;
      const float half = waveHeight * 0.44f;

      // --- the lane as a block of its own ---
      //
      // Each track gets a slab, with a gap either side. Contiguous lanes divided by a hairline read
      // as one table with rules in it; separate blocks read as separate takes, which is what they
      // are.
      //
      // The shade is the accent, and it fades down the stack - strongest at the top, weakest at
      // the bottom. A repeating cycle of shades made neighbours differ but said nothing, and the
      // pair that came round again looked like the same track twice; a gradient says where in the
      // stack a lane is, which is the one thing about its position worth knowing at a glance.
      const float depth = (tracks.size() > 1) ? static_cast<float>(t) / static_cast<float>(tracks.size() - 1) : 0.0f;
      const float shade = 0.20f - 0.155f * depth;

      constexpr float kLaneGap = 4.0f;
      const ImVec2 slabFrom(origin.x + 1.0f, top + kLaneGap * 0.5f);
      const ImVec2 slabTo(area.x - 1.0f, top + laneHeight - kLaneGap * 0.5f);
      const ImVec4 accent = theme::Accent();
      draw->AddRectFilled(slabFrom, slabTo,
                          ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z,
                                                    audibleLane ? shade : shade * 0.4f)),
                          6.0f);

      // The lane half of the selection outline. Its left edge is left open, where the controls'
      // half continues it - together they are one rectangle round the channel, and neither is
      // drawn on the foreground list, so a window opened over the timeline covers both.
      if (mSelectedTrackId == track.id)
      {
        const ImU32 edge = ImGui::GetColorU32(theme::Accent());
        draw->AddLine(ImVec2(origin.x, top + 1.0f), ImVec2(area.x - 1.0f, top + 1.0f), edge, 1.5f);
        draw->AddLine(ImVec2(origin.x, top + laneHeight - 1.0f), ImVec2(area.x - 1.0f, top + laneHeight - 1.0f), edge,
                      1.5f);
        draw->AddLine(ImVec2(area.x - 1.0f, top + 1.0f), ImVec2(area.x - 1.0f, top + laneHeight - 1.0f), edge, 1.5f);
      }

      if (showNotes)
        DrawNoteLane(found->second, top + waveHeight, noteHeight, origin.x, area.x, viewStart, viewSpan, hovered);

      if (!track.peaks || track.peaks->Empty())
        continue;

      // Dimmed when it is not going to be heard, so mute and solo are visible on the waveform
      // rather than only in the list.
      const ImU32 colour = ImGui::GetColorU32(ImVec4(1, 1, 1, audibleLane ? kWaveAudible : kWaveSilent));

      // One vertical line per pixel column. The cache answers it at normal zoom; past the point
      // where a bucket covers less than a column the cache has nothing finer to give, so the
      // samples are read directly - which is only affordable because by then there are few of them.
      const int columns = static_cast<int>(width);
      const size_t totalFrames = track.file->FrameCount();
      const double visibleFrames = viewSpan * track.file->sampleRate;
      const bool readDirectly = visibleFrames < 200000.0; // about four seconds at 48 kHz

      for (int column = 0; column < columns; column++)
      {
        const double columnFrom = viewStart + viewSpan * static_cast<double>(column) / static_cast<double>(columns);
        const double columnTo = viewStart + viewSpan * static_cast<double>(column + 1) / static_cast<double>(columns);

        float low = 0.0f;
        float high = 0.0f;
        if (readDirectly)
        {
          track.file->PeakBetween(static_cast<size_t>(std::max(0.0, columnFrom) * track.file->sampleRate),
                                  static_cast<size_t>(std::max(0.0, columnTo) * track.file->sampleRate), low, high);
        }
        else if (duration > 0.0)
        {
          const PeakCache::Peak peak = track.peaks->Range(columnFrom / duration, columnTo / duration);
          low = peak.low;
          high = peak.high;
        }

        const float x = origin.x + static_cast<float>(column);
        draw->AddLine(ImVec2(x, centre - high * half), ImVec2(x, centre - low * half), colour);
      }
      (void)totalFrames;
    }

    // --- the click's lane, under the tracks ---
    //
    // It has no waveform to show, so it shows what it plays: a mark on every beat, standing on the
    // same grid the ruler numbers. Taller and full strength on a downbeat, short and quiet on the
    // rest, which is the click itself drawn out along the song.
    {
      const float top = lanesTop - mLaneScroll + laneHeight * static_cast<float>(tracks.size());
      if (top + laneHeight >= lanesTop && top <= area.y)
      {
        constexpr float kLaneGap = 4.0f;
        const ImVec4 accent = theme::Accent();
        draw->AddRectFilled(ImVec2(origin.x + 1.0f, top + kLaneGap * 0.5f),
                            ImVec2(area.x - 1.0f, top + laneHeight - kLaneGap * 0.5f),
                            ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, mClickArmed ? 0.055f : 0.022f)),
                            6.0f);

        const std::vector<double> beats = BeatTimes();
        const int beatsPerBar = std::max(1, mGridBeatsPerBar);
        // The model marks every downbeat itself. Counting them off with a fixed bar length instead
        // drifts against the music the moment the meter is not what the project happens to say, and
        // then the tall marks land on beats nothing happens on - which is what made an even row of
        // beats read as though they were unevenly placed.
        //
        // Used only when there is a flag for every beat, so a stale or half-filled list falls back
        // to counting rather than marking whatever happens to line up with its length.
        static const std::vector<unsigned char> kNoFlags;
        const std::vector<unsigned char>& flags =
          (mTempo.downbeats.size() == mTempo.beats.size()) ? mTempo.downbeats : kNoFlags;
        const float centre = top + laneHeight * 0.5f;
        const float full = std::min(laneHeight * 0.34f, 22.0f);

        // Drawn only where they are far enough apart to be told from each other. Zoomed out, a
        // mark per beat is a solid block that says nothing about where the beats are.
        const double perPixel = viewSpan / static_cast<double>(std::max(1.0f, width));
        const bool everyBeat = beats.size() < 2 || (beats[1] - beats[0]) / perPixel > 5.0;

        for (size_t i = 0; i < beats.size(); i++)
        {
          const bool downbeat =
            (i < flags.size())
              ? flags[i] != 0
              : ((static_cast<long long>(i) - mTempo.downbeatOffset) % beatsPerBar + beatsPerBar) % beatsPerBar == 0;
          if (!everyBeat && !downbeat)
            continue;
          if (beats[i] < viewStart || beats[i] > viewStart + viewSpan)
            continue;

          const float x = xForTime(beats[i]);
          const float half = downbeat ? full : full * 0.5f;
          const float alpha = (mClickArmed ? 1.0f : 0.35f) * (downbeat ? 0.9f : 0.45f);
          draw->AddLine(ImVec2(x, centre - half), ImVec2(x, centre + half),
                        ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, alpha)), downbeat ? 2.0f : 1.4f);
        }

        if (beats.empty())
        {
          const char* none = "No tempo yet";
          const ImVec2 size = ImGui::CalcTextSize(none);
          draw->AddText(ImVec2(origin.x + 14.0f, centre - size.y * 0.5f), ImGui::GetColorU32(theme::TextFaint()), none);
        }

        // --- what you would tap, over the beats it changes ---
        //
        // Whether a song is at 84 or 168 is a question about counting, not about the audio: both
        // are true of the same recording. Put here rather than among the channel's controls because
        // what it changes is these marks, and you press it while looking at them.
        //
        // Small and quiet: it is on top of the thing it is about, so anything louder would be in
        // the way of what you are trying to read.
        if (!beats.empty())
        {
          static const float kMultiples[] = {0.5f, 1.0f, 2.0f};
          static const char* kLabels[] = {"½", "1", "2"};

          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
          ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, top + 7.0f));

          for (int i = 0; i < 3; i++)
          {
            if (i > 0)
              ImGui::SameLine(0, 2);
            ImGui::PushID(i);

            const bool here = std::fabs(mGridMultiple - kMultiples[i]) < 1.0e-3f;
            ImGui::PushStyleColor(ImGuiCol_Text, here ? theme::Accent() : theme::TextFaint());
            ImGui::PushStyleColor(ImGuiCol_Button, here ? theme::AccentDim() : ImVec4(0, 0, 0, 0));
            const bool pressed = ImGui::Button(kLabels[i], ImVec2(22.0f, 0.0f));
            ImGui::PopStyleColor(2);

            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(i == 0   ? "Count half as often"
                                : i == 1 ? "Count as it was detected"
                                         : "Count twice as often");

            if (pressed && !here)
            {
              // Applied as the step from where it is now, so the grid is only ever halved or
              // doubled - which is what keeps the beats on the beats.
              for (float at = mGridMultiple; at > kMultiples[i] + 1.0e-3f; at *= 0.5f)
                mTempo.Halve();
              for (float at = mGridMultiple; at < kMultiples[i] - 1.0e-3f; at *= 2.0f)
                mTempo.Double();

              mGridMultiple = kMultiples[i];
              SendTempoToMetronome();
              RebuildSynthScore();
            }

            ImGui::PopID();
          }

          ImGui::PopStyleVar(2);
        }
      }
    }

    draw->PopClipRect();
  }
  else
  {
    // --- nothing loaded yet ---
    //
    // The two ways in, where the tracks would be, rather than a line of text telling you to go and
    // find the menu. An empty screen whose only words are a description of itself is a dead end;
    // these are the same two actions the project menu offers, put in the hands' way.
    const char* labels[2] = {"Import audio", "Open project"};
    ImVec2 boxes[2];
    const ImVec2 padding(22.0f, 10.0f);
    constexpr float kGap = 12.0f;

    float total = kGap;
    for (int i = 0; i < 2; i++)
    {
      const ImVec2 size = ImGui::CalcTextSize(labels[i]);
      boxes[i] = ImVec2(size.x + padding.x * 2.0f, size.y + padding.y * 2.0f);
      total += boxes[i].x;
    }

    float x = origin.x + (width - total) * 0.5f;
    const float y = lanesTop + (lanesHeight - boxes[0].y) * 0.5f;

    for (int i = 0; i < 2; i++)
    {
      const ImVec2 at(x, y);
      ImGui::SetCursorScreenPos(at);
      // A quiet outline, the same offer the Separate stems button makes: with no fill at rest a
      // button on an empty black field has no edge at all and reads as a caption.
      draw->AddRect(at, ImVec2(at.x + boxes[i].x, at.y + boxes[i].y), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.22f)),
                    6.0f, 0, 1.0f);
      if (ImGui::Button(labels[i], boxes[i]))
      {
        if (i == 0)
          AddPlayerTrack();
        else
          BrowseAndOpenProject();
      }
      x += boxes[i].x + kGap;
    }
  }


  // No grid over the waveforms. The bars are numbered along the ruler, which is where you read
  // them; ruling every lane as well drew the same information six more times, over the top of the
  // thing you are actually looking at.

  // --- the ruler ---
  //
  // Numbered in bars where the song has a grid and in minutes where it does not, because "bar 17"
  // is what a part is called and "1:04" is only where it happens to fall.
  const float rulerSplit = origin.y + 16.0f; // numbers above, loop handles below
  draw->AddRectFilled(origin, ImVec2(area.x, origin.y + kRulerHeight), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f)));
  draw->AddLine(ImVec2(origin.x, origin.y + kRulerHeight), ImVec2(area.x, origin.y + kRulerHeight),
                ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)));

  {
    const ImU32 tickColour = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.35f));
    const ImU32 fineColour = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.15f));
    const ImU32 textColour = ImGui::GetColorU32(theme::TextDim());

    // With the grid switched off the ruler goes back to counting seconds. A song has a bar 47 and a
    // 2:31 both; which of the two you want depends on what you are doing to it, and this is the
    // switch between them rather than a second row showing both.
    if (mShowGrid && mTempo.valid && mTempo.bpm > 0.0f)
    {
      // Walked beat by beat off the same map the grid lines use, so a bar number and the bar line
      // under it can never disagree - which they would the moment the two extrapolated separately.
      const long long firstIndex = mTempo.BeatIndexAt(viewStart) - beatsPerBar;
      const long long lastIndex = mTempo.BeatIndexAt(viewStart + viewSpan) + 1;

      // Label every bar while they are far apart, every second or fourth as they close up. Numbers
      // that overlap are worse than no numbers at all.
      const double barSeconds = mTempo.BeatLength(std::max<long long>(0, firstIndex)) * beatsPerBar;
      const float barPixels = static_cast<float>(barSeconds / viewSpan) * width;
      int step = 1;
      while (barPixels * static_cast<float>(step) < 46.0f && step < 1024)
        step *= 2;

      for (long long index = std::max<long long>(0, firstIndex); index <= lastIndex; index++)
      {
        const double at = mTempo.BeatTime(index);
        if (at < 0.0 || at > duration)
          continue;

        const float x = xForTime(at);
        const float beatPixels = static_cast<float>(mTempo.BeatLength(index) / viewSpan) * width;

        if (!isBarLine(index))
        {
          // Beat ticks, once there is room for them to mean anything.
          if (beatPixels >= 10.0f)
            draw->AddLine(ImVec2(x, rulerSplit - 4.0f), ImVec2(x, rulerSplit), fineColour);
          continue;
        }

        const long long bar = barNumberOf(index);
        if (((bar - 1) % step) != 0)
        {
          draw->AddLine(ImVec2(x, rulerSplit - 6.0f), ImVec2(x, rulerSplit), fineColour);
          continue;
        }

        draw->AddLine(ImVec2(x, origin.y + 2.0f), ImVec2(x, rulerSplit), tickColour);
        char label[16];
        std::snprintf(label, sizeof(label), "%lld", bar);
        draw->AddText(ImVec2(x + 4.0f, origin.y + 1.0f), textColour, label);
      }
    }
    else if (duration > 0.0)
    {
      // No grid, either because none was found or because it is switched off: a time ruler,
      // stepping through the intervals a clock actually uses.
      static const double kSteps[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0};
      double step = kSteps[0];
      for (const double candidate : kSteps)
      {
        step = candidate;
        if (static_cast<float>(candidate / viewSpan) * width >= 70.0f)
          break;
      }

      const long long first = static_cast<long long>(std::floor(viewStart / step));
      const long long last = static_cast<long long>(std::ceil((viewStart + viewSpan) / step));
      for (long long index = std::max<long long>(0, first); index <= last; index++)
      {
        const double at = static_cast<double>(index) * step;
        if (at > duration)
          break;
        const float x = xForTime(at);
        draw->AddLine(ImVec2(x, origin.y + 2.0f), ImVec2(x, rulerSplit), tickColour);
        draw->AddText(ImVec2(x + 4.0f, origin.y + 1.0f), textColour, FormatTime(at).c_str());
      }
    }
  }

  // --- loops over the waveforms, with a handle in the ruler ---
  const int activeLoop = player.GetActiveLoop();
  for (size_t i = 0; i < loops.size(); i++)
  {
    const Loop& loop = loops[i];
    if (!loop.Valid())
      continue;

    const float left = xForTime(loop.startSeconds);
    const float right = xForTime(loop.endSeconds);
    const bool active = (static_cast<int>(i) == activeLoop);
    const ImVec4 colour = active ? theme::Accent() : theme::Control();

    draw->AddRectFilled(ImVec2(left, lanesTop), ImVec2(right, area.y),
                        ImGui::GetColorU32(ImVec4(colour.x, colour.y, colour.z, active ? 0.16f : 0.07f)));
    draw->AddLine(ImVec2(left, origin.y), ImVec2(left, area.y), ImGui::GetColorU32(colour), active ? 2.0f : 1.0f);
    draw->AddLine(ImVec2(right, origin.y), ImVec2(right, area.y), ImGui::GetColorU32(colour), active ? 2.0f : 1.0f);

    // The solid bar in the ruler is the thing to grab: it is there at every zoom, even when the
    // loop itself is a sliver, and it is where a loop is dragged from.
    const ImVec2 handleFrom(left, rulerSplit + 1.0f);
    const ImVec2 handleTo(right, origin.y + kRulerHeight - 2.0f);
    draw->AddRectFilled(handleFrom, handleTo,
                        ImGui::GetColorU32(ImVec4(colour.x, colour.y, colour.z, active ? 0.95f : 0.45f)), 2.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(loop.name.c_str());
    if (right - left > textSize.x + 10.0f)
    {
      // Inside the handle when it fits: dark over the filled accent of the active loop, ordinary
      // text over the faded bar of the others.
      const ImU32 nameColour =
        active ? ImGui::GetColorU32(ImVec4(0.05f, 0.06f, 0.08f, 0.95f)) : ImGui::GetColorU32(ImGuiCol_Text);
      draw->AddText(ImVec2(left + 5.0f, handleFrom.y + (handleTo.y - handleFrom.y - textSize.y) * 0.5f), nameColour,
                    loop.name.c_str());
    }
  }

  // --- what a loop can be told to do ---
  //
  // On the loop itself rather than in a list beside it: the handle in the ruler is where the loop
  // is, and a menu there needs no list to exist at all.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
  {
    for (size_t i = 0; i < loops.size(); i++)
    {
      if (!loops[i].Valid())
        continue;
      const float pointerX = ImGui::GetIO().MousePos.x;
      if (pointerX >= xForTime(loops[i].startSeconds) && pointerX <= xForTime(loops[i].endSeconds))
      {
        mLoopMenuIndex = static_cast<int>(i);
        std::snprintf(mLoopNameInput, sizeof(mLoopNameInput), "%s", loops[i].name.c_str());
        ImGui::OpenPopup("##loopmenu");
        break;
      }
    }
  }

  if (ImGui::BeginPopup("##loopmenu"))
  {
    auto current = player.GetLoops();
    if (mLoopMenuIndex >= 0 && static_cast<size_t>(mLoopMenuIndex) < current.size())
    {
      Loop& loop = current[static_cast<size_t>(mLoopMenuIndex)];

      ImGui::SetNextItemWidth(200);
      if (ImGui::InputText("##loopname", mLoopNameInput, sizeof(mLoopNameInput),
                           ImGuiInputTextFlags_EnterReturnsTrue))
      {
        loop.name = mLoopNameInput;
        player.SetLoops(current);
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
      {
        loop.name = mLoopNameInput;
        player.SetLoops(current);
      }

      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint());
      ImGui::Text("%s  -  %s", FormatTime(loop.startSeconds).c_str(), FormatTime(loop.endSeconds).c_str());
      ImGui::PopStyleColor();

      ImGui::Separator();
      if (ImGui::MenuItem("Play this loop"))
      {
        player.SetActiveLoop(mLoopMenuIndex);
        player.SetPositionSeconds(loop.startSeconds);
        mSelectedLoop = mLoopMenuIndex;
      }
      if (ImGui::MenuItem("Zoom to it"))
      {
        const double margin = std::max(0.25, (loop.endSeconds - loop.startSeconds) * 0.1);
        mViewStart = std::max(0.0, loop.startSeconds - margin);
        mViewDuration = (loop.endSeconds - loop.startSeconds) + margin * 2.0;
      }

      ImGui::Separator();
      if (ImGui::MenuItem("Delete"))
      {
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(mLoopMenuIndex));
        player.SetLoops(current);
        player.SetActiveLoop(-1);
        mSelectedLoop = -1;
        mLoopMenuIndex = -1;
      }
    }
    ImGui::EndPopup();
  }

  // --- the selection being dragged out ---
  //
  // Drawn where it will actually land, snapped and all. Showing the raw drag and only snapping on
  // release means the region jumps at the last moment, which reads as the app moving your loop
  // rather than as the loop locking to the beat.
  if (mLoopDragStart >= 0.0)
  {
    const float left = xForTime(SnapToBeat(std::min(mLoopDragStart, mLoopDragEnd)));
    const float right = xForTime(SnapToBeat(std::max(mLoopDragStart, mLoopDragEnd)));
    draw->AddRectFilled(ImVec2(left, origin.y), ImVec2(right, area.y),
                        ImGui::GetColorU32(ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.22f)));
    draw->AddLine(ImVec2(left, origin.y), ImVec2(left, area.y), ImGui::GetColorU32(theme::Accent()), 1.5f);
    draw->AddLine(ImVec2(right, origin.y), ImVec2(right, area.y), ImGui::GetColorU32(theme::Accent()), 1.5f);
  }

  // --- playhead ---
  if (duration > 0.0)
  {
    const float x = xForTime(player.GetPositionSeconds());
    // The one moving thing on the screen, and the only thing on the timeline that is coloured.
    // Against white waveforms it separates instantly; in white it would be lost in them.
    const ImU32 colour = ImGui::GetColorU32(theme::Accent());
    draw->AddLine(ImVec2(x, origin.y), ImVec2(x, area.y), colour, 2.0f);
    // A flag in the ruler, so the position is findable when the line is lost in a busy waveform.
    draw->AddTriangleFilled(ImVec2(x - 5.0f, origin.y), ImVec2(x + 5.0f, origin.y), ImVec2(x, origin.y + 7.0f),
                            colour);
  }

  draw->PopClipRect();

  // --- zoom and pan ---
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  // Separating stems is offered in the first channel's own controls, beside its level - the same
  // place everything else about that channel is done - rather than as a box lying over the
  // waveform, which put the one thing the screen is for behind an invitation.

  // No zooming here. The wheel scrolls - up and down over the lanes, sideways with Shift - and the
  // scrollbars do the zooming, which is the whole of the rule and needs no modifier to remember.

  // Middle-drag still pans, because it costs nothing and it is what a hand reaches for. Left is
  // taken by loops and right opens the loop menu, so the wheel button is the one left over.
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
  {
    const double perPixel = viewSpan / static_cast<double>(width);
    mViewStart = std::clamp(mViewStart - static_cast<double>(ImGui::GetIO().MouseDelta.x) * perPixel, 0.0,
                            std::max(0.0, duration - viewSpan));
  }

  // --- interaction ---
  //
  // Left takes hold of what is already there: near an edge it resizes, inside a loop it moves it,
  // and anywhere else it is a seek. Marking out a new loop is the right button - a left drag over
  // a waveform is a selection everywhere else, and here it was making a loop out of every attempt
  // to scrub.
  const float kEdgeGrab = 6.0f;

  if (hovered && duration > 0.0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
  {
    mLoopEdit = LoopEdit::None;
    mEditedLoop = -1;

    for (size_t i = 0; i < loops.size() && mLoopEdit == LoopEdit::None; i++)
    {
      if (!loops[i].Valid())
        continue;
      const float left = xForTime(loops[i].startSeconds);
      const float right = xForTime(loops[i].endSeconds);

      if (std::fabs(mouse.x - left) <= kEdgeGrab)
      {
        mLoopEdit = LoopEdit::Start;
        mEditedLoop = static_cast<int>(i);
      }
      else if (std::fabs(mouse.x - right) <= kEdgeGrab)
      {
        mLoopEdit = LoopEdit::End;
        mEditedLoop = static_cast<int>(i);
      }
      else if (mouse.x > left && mouse.x < right)
      {
        mLoopEdit = LoopEdit::Move;
        mEditedLoop = static_cast<int>(i);
        mLoopGrabOffset = timeForX(mouse.x) - loops[i].startSeconds;
      }
    }

    if (mLoopEdit == LoopEdit::None && !mNoteAudition)
    {
      // A click on empty timeline is a seek, and a seek is not something to snap - you meant that
      // spot. Remembered as well, so 0 on the keyboard comes back here.
      //
      // Unless it landed on a note or a chord, which was already sounded: pressing one to hear it
      // and having the playhead jump as well would be two answers to one press.
      const double at = std::clamp(timeForX(mouse.x), 0.0, duration);
      player.SetPositionSeconds(at);
      mLastSeekSeconds = at;
      mFollowPlayhead = false;
    }
    else
    {
      mSelectedLoop = mEditedLoop;
    }
  }

  // The right button marks one out.
  if (hovered && duration > 0.0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mLoopMenuIndex < 0)
  {
    mLoopDragStart = timeForX(mouse.x);
    mLoopDragEnd = mLoopDragStart;
  }

  // The pointer says what a press would do before you commit to it.
  if (hovered && mLoopEdit == LoopEdit::None && mLoopDragStart < 0.0)
  {
    for (const auto& loop : loops)
    {
      if (!loop.Valid())
        continue;
      if (std::fabs(mouse.x - xForTime(loop.startSeconds)) <= kEdgeGrab
          || std::fabs(mouse.x - xForTime(loop.endSeconds)) <= kEdgeGrab)
      {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        break;
      }
    }
  }

  // --- editing an existing loop ---
  if (mLoopEdit != LoopEdit::None && mEditedLoop >= 0 && static_cast<size_t>(mEditedLoop) < loops.size())
  {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      Loop& loop = loops[static_cast<size_t>(mEditedLoop)];
      const double at = SnapToBeat(timeForX(mouse.x));

      if (mLoopEdit == LoopEdit::Start)
      {
        loop.startSeconds = std::min(at, loop.endSeconds - 0.05);
      }
      else if (mLoopEdit == LoopEdit::End)
      {
        loop.endSeconds = std::max(at, loop.startSeconds + 0.05);
      }
      else
      {
        // Moved by its start, so the length is preserved exactly rather than being rebuilt from
        // two independently snapped edges.
        const double length = loop.endSeconds - loop.startSeconds;
        const double start = std::max(0.0, SnapToBeat(timeForX(mouse.x) - mLoopGrabOffset));
        loop.startSeconds = start;
        loop.endSeconds = start + length;
      }
      player.SetLoops(loops);
    }
    else
    {
      mLoopEdit = LoopEdit::None;
      mEditedLoop = -1;
    }
  }

  if (mLoopDragStart >= 0.0 && ImGui::IsMouseDown(ImGuiMouseButton_Right))
    mLoopDragEnd = timeForX(mouse.x);

  if (mLoopDragStart >= 0.0 && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
  {
    const double rawFrom = std::min(mLoopDragStart, mLoopDragEnd);
    const double rawTo = std::max(mLoopDragStart, mLoopDragEnd);

    double from = SnapToBeat(rawFrom);
    double to = SnapToBeat(rawTo);

    // A drag shorter than half a beat snaps both edges onto the same one. It was still a drag, so
    // give it the beat it was reaching for rather than throwing it away as a click.
    const bool snapping = mSnapToGrid && mTempo.valid && mTempo.bpm > 0.0f;
    if (snapping && to <= from)
      to = mTempo.BeatTime(mTempo.BeatIndexAt(from) + 1);

    // Under a fifth of a second of travel is a click that wandered, not a selection. Measured on
    // what the mouse did, not on what the snap made of it.
    if (rawTo - rawFrom > 0.2)
    {
      Loop loop;
      loop.name = "Loop " + std::to_string(loops.size() + 1);
      loop.startSeconds = from;
      loop.endSeconds = to;
      loops.push_back(loop);
      player.SetLoops(loops);
      mSelectedLoop = static_cast<int>(loops.size()) - 1;
      player.SetActiveLoop(mSelectedLoop);
      player.SetPositionSeconds(from);
    }
    // A right-click that did not travel is not a loop. It falls through to the loop menu, which
    // opens on the same button, so nothing else happens here.

    mLoopDragStart = -1.0;
    mLoopDragEnd = -1.0;
  }
}

/// \brief Gives the metronome the song's tempo, scaled by how fast the song is being played.
///
/// There is no button for this. The click is a channel of the song now, so its tempo is the song's
/// tempo - and a button to say so was a way of leaving the two disagreeing until it was pressed.
///
/// Speed is part of it: at 80% the record plays slower and its beats arrive further apart, so a
/// click at the written tempo walks straight off the music.
void MainWindow::SendTempoToMetronome()
{
  if (!mTempo.valid || mTempo.bpm <= 0.0f)
    return;

  const float speed = std::clamp(mEngine.GetPlayer().GetSpeed(), kMinPlaybackSpeed, kMaxPlaybackSpeed);
  const float wanted = std::clamp(mTempo.bpm * speed, 20.0f, 300.0f);

  if (std::fabs(mMetronome.bpm - wanted) < 0.01f && mMetronome.beatsPerBar == mGridBeatsPerBar)
    return;

  mMetronome.bpm = wanted;
  mMetronome.beatsPerBar = mGridBeatsPerBar;
  ApplyMetronome();
}

void MainWindow::SyncMetronomeToSong()
{
  if (!mTempo.valid || mTempo.bpm <= 0.0f)
    return;

  Player& player = mEngine.GetPlayer();
  const double position = player.GetPositionSeconds();
  mLastClickSync = position;

  // The tempo *here*, not the song's average. On a song that drifts those are different numbers,
  // and running the click at the average is what makes it walk away from the music.
  //
  // Scaled by the playback speed for the same reason: at 80% the beats arrive further apart than
  // the record was played at, and a click at the written tempo would be counting a song nobody is
  // hearing.
  const long long index = mTempo.BeatIndexAt(position);
  const double length = mTempo.BeatLength(index);
  const float speed = std::clamp(player.GetSpeed(), kMinPlaybackSpeed, kMaxPlaybackSpeed);

  mMetronome.bpm =
    std::clamp((length > 0.0) ? static_cast<float>(60.0 / length) * speed : mTempo.bpm * speed, 20.0f, 300.0f);
  mMetronome.beatsPerBar = mGridBeatsPerBar;
  mClickSpeed = speed;
  ApplyMetronome();

  // How far into this beat the playhead is, and which beat of the bar it is. Taken from the model's
  // own downbeats when it marked them, so the accent lands on the bar line the grid draws.
  const double into = (length > 0.0) ? std::clamp((position - mTempo.BeatTime(index)) / length, 0.0, 0.999999) : 0.0;

  int beatIndex = 0;
  if (!mTempo.downbeats.empty() && index >= 0 && static_cast<size_t>(index) < mTempo.downbeats.size())
  {
    for (long long back = index; back >= 0; back--)
    {
      if (mTempo.downbeats[static_cast<size_t>(back)] == 0)
        continue;
      beatIndex = static_cast<int>(index - back);
      break;
    }
    beatIndex %= std::max(1, mGridBeatsPerBar);
  }
  else
  {
    const int beats = std::max(1, mGridBeatsPerBar);
    const long long fromDownbeat = index - mTempo.downbeatOffset;
    beatIndex = static_cast<int>(((fromDownbeat % beats) + beats) % beats);
  }

  mEngine.SyncMetronome(into, beatIndex);
}

void MainWindow::DrawTransportBar()
{
  Player& player = mEngine.GetPlayer();
  const double duration = player.DurationSeconds();
  const double position = player.GetPositionSeconds();
  auto loops = player.GetLoops();

  // --- the click as a mode ---
  //
  // Armed here and running only while the song is. A metronome that carries on after you stop is
  // a metronome you have to switch off twice, and one you then have to start in time by hand.
  const bool clickShouldRun = mClickArmed && player.IsPlaying() && mTempo.valid;
  if (clickShouldRun && !mMetronomeRunning)
  {
    mMetronomeRunning = true;
    mClickFromTransport = true;
    SyncMetronomeToSong(); // joins the song wherever the playhead is
  }
  else if (!clickShouldRun && mMetronomeRunning && mClickFromTransport)
  {
    // Only ever stops what it started: a click running from the metronome view is that view's.
    mMetronomeRunning = false;
    mClickFromTransport = false;
    ApplyMetronome();
  }

  // Any jump in the playhead - a seek, a loop starting over, a new loop selected - puts the click
  // out of step, so it is put back. Detected here rather than at each of the dozen places that
  // can move the position, which is one place to get right instead of twelve.
  if (mMetronomeRunning && mTempo.valid)
  {
    const double travelled = position - mLastPlayerPosition;
    const float speed = std::clamp(player.GetSpeed(), kMinPlaybackSpeed, kMaxPlaybackSpeed);

    if (travelled < 0.0 || travelled > 0.5)
    {
      SyncMetronomeToSong();
    }
    else if (std::fabs(speed - mClickSpeed) > 1.0e-4f)
    {
      // The song is being played at a different speed than the click was set for. Re-joined rather
      // than merely re-tempoed, so it lands back on the beat it should be on instead of carrying
      // its old phase into the new tempo.
      SyncMetronomeToSong();
    }
    else if (!mTempo.beats.empty() && std::fabs(position - mLastClickSync) > 1.0)
    {
      // A tracked grid means the song's tempo moves, and a click set once at the average would
      // walk away from it. Put back onto the beat every second: the correction is a millisecond or
      // two at a time, which is a click that stays with the music rather than one that is nudged.
      SyncMetronomeToSong();
    }
  }
  mLastPlayerPosition = position;

  // --- the transport ---
  //
  // Laid out by hand rather than flowed, and centred on the window: this is the one row that gets
  // looked at with a guitar in your hands, so it is built around a single obvious target with the
  // rest arranged around it by importance. Play is the largest thing and the only filled one.
  const ImVec2 keySize(40.0f, 36.0f);
  const ImVec2 playSize(56.0f, 36.0f);
  constexpr float kGap = 8.0f;

  const float barWidth = ImGui::GetContentRegionAvail().x;
  const ImVec2 barOrigin = ImGui::GetCursorScreenPos();
  const float bayHeight = playSize.y;
  const float centreY = barOrigin.y + bayHeight * 0.5f;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const auto placeAt = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, centreY - h * 0.5f)); };

  // No container behind the keys. Spacing groups them; a box around them would only be one more
  // shape to look at.
  const float clusterWidth = keySize.x * 4.0f + playSize.x + kGap * 4.0f;
  float x = barOrigin.x + (barWidth - clusterWidth) * 0.5f;

  // --- master level, behind the speaker ---
  //
  // A fader that is set once and then left alone does not need to be on screen the whole time it
  // is not being set. The icon says what it is; the fader appears over it when reached for.
  {
    placeAt(x - keySize.x - 18.0f, keySize.y);
    float playerGain = player.GetGainDb();
    const bool quiet = playerGain <= -39.5f;

    theme::TransportKey("##volume", theme::Icon::Volume, keySize, false, quiet);

    // Allowed through while the popup is up, because that is exactly when it would otherwise say
    // the icon is not hovered and close the thing you are reaching into.
    const bool overIcon = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    const ImVec2 iconMin = ImGui::GetItemRectMin();

    // A popup rather than something drawn by hand. The fader stands above the transport, over the
    // timeline - and the timeline is a child window, so anything drawn into this one lands behind
    // it and can never be hovered. A popup is a window of its own, which is what puts it in front.
    if (overIcon && !ImGui::IsPopupOpen("##volumepop"))
      ImGui::OpenPopup("##volumepop");

    constexpr float kFaderHeight = 116.0f;
    // Sat right on top of the icon rather than floating above it, so there is barely a gap to
    // cross in the first place.
    ImGui::SetNextWindowPos(ImVec2(iconMin.x + keySize.x * 0.5f, iconMin.y + 1.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 17.0f);
    if (ImGui::BeginPopup("##volumepop", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
    {
      if (theme::SlimSliderVertical("##playergain", &playerGain, -40.0f, 12.0f, 0.0f, kFaderHeight, "%+.1f dB"))
        player.SetGainDb(playerGain);

      const bool inside = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                                                 | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

      // The corridor between the two. A pointer moving from the icon up to the fader passes over
      // neither of them for a frame or two, and cutting the corner takes it wider still - so the
      // whole column they share counts as being on it.
      const ImVec2 popMin = ImGui::GetWindowPos();
      const ImVec2 popMax(popMin.x + ImGui::GetWindowSize().x, popMin.y + ImGui::GetWindowSize().y);
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const bool inCorridor = mouse.x >= std::min(popMin.x, iconMin.x) - 12.0f
                              && mouse.x <= std::max(popMax.x, iconMin.x + keySize.x) + 12.0f
                              && mouse.y >= popMin.y - 12.0f && mouse.y <= iconMin.y + keySize.y + 12.0f;

      // And a moment's grace on top, so a hand that overshoots gets to come back.
      if (inside || overIcon || inCorridor || ImGui::IsAnyItemActive())
        mVolumeGrace = 0.0f;
      else
        mVolumeGrace += ImGui::GetIO().DeltaTime;

      if (mVolumeGrace > 0.4f)
      {
        mVolumeGrace = 0.0f;
        ImGui::CloseCurrentPopup();
      }

      ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
  }

  placeAt(x, keySize.y);
  if (theme::TransportKey("##tostart", theme::Icon::SkipStart, keySize))
    player.SetPositionSeconds(0.0);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Back to the start (Home)");
  x += keySize.x + kGap;

  placeAt(x, keySize.y);
  if (theme::TransportKey("##back", theme::Icon::Rewind, keySize))
    player.SetPositionSeconds(position - 5.0);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Back five seconds (Left)");
  x += keySize.x + kGap;

  placeAt(x, playSize.y);
  const bool playing = player.IsPlaying();
  if (theme::TransportKey("##play", playing ? theme::Icon::Pause : theme::Icon::Play, playSize, playing))
    player.TogglePlay();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(playing ? "Pause (Space)" : "Play (Space)");
  x += playSize.x + kGap;

  placeAt(x, keySize.y);
  if (theme::TransportKey("##stop", theme::Icon::Stop, keySize))
    player.Stop();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Stop, and back to the top of the loop (0)");
  x += keySize.x + kGap;

  placeAt(x, keySize.y);
  if (theme::TransportKey("##fwd", theme::Icon::Forward, keySize))
    player.SetPositionSeconds(std::min(position + 5.0, duration));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("On five seconds (Right)");

  // --- what the timeline does, out at the left ---
  //
  // The loop, and three switches that used to live two clicks deep in a panel. They belong on the
  // bar: each is something you reach for while looking at the waveform, and lit or unlit they say
  // how the timeline is behaving without anything having to be opened to find out.
  //
  // All of them armed in the accent rather than filled white. White is the strongest thing this
  // palette can do and the bar spends it on Play alone; four white squares in a row next to it and
  // the eye no longer knows which one is the button that matters. These are modes, and a mode being
  // on is exactly what the accent is for.
  //
  // Only where there is a timeline. On the tuner or the rig they would be four controls over
  // something that is not on the screen.
  if (mView == View::Player)
  {
    float left = barOrigin.x + 6.0f;

    // --- the loop, and the whole set of them behind it ---
    //
    // One key where there used to be four and a name. Pressed, it starts and stops the loop that is
    // running; held, it opens the list and you pick from it. The arrows and the name they sat
    // either side of were three controls for a thing most songs have two or three of, taking up the
    // whole right-hand end of the bar to say so.
    {
      const int active = player.GetActiveLoop();
      placeAt(left, keySize.y);
      theme::TransportKey("##loop", theme::Icon::Loop, keySize, false, active >= 0);

      const ImVec2 loopMin = ImGui::GetItemRectMin();
      const bool loopHovered = ImGui::IsItemHovered();

      // Driven from press and release rather than from the button's own click, which fires on the
      // way down - a long press would otherwise toggle the loop before it opened the list.
      if (ImGui::IsItemActivated())
      {
        mLoopPressAt = ImGui::GetTime();
        mLoopMenuFromHold = false;
      }

      constexpr double kHoldSeconds = 0.35;
      if (ImGui::IsItemActive() && !mLoopMenuFromHold && !loops.empty()
          && ImGui::GetTime() - mLoopPressAt > kHoldSeconds)
      {
        mLoopMenuFromHold = true;
        ImGui::OpenPopup("##looppick");
      }

      // A press that ended before the list opened is the plain toggle: off if one is running,
      // otherwise back to the one you last touched.
      if (ImGui::IsItemDeactivated() && !mLoopMenuFromHold)
      {
        if (active >= 0)
        {
          player.SetActiveLoop(-1);
        }
        else if (!loops.empty())
        {
          const int wanted =
            (mSelectedLoop >= 0 && static_cast<size_t>(mSelectedLoop) < loops.size()) ? mSelectedLoop : 0;
          player.SetActiveLoop(wanted);
          player.SetPositionSeconds(loops[static_cast<size_t>(wanted)].startSeconds);
        }
      }

      if (loopHovered && !ImGui::IsPopupOpen("##looppick"))
      {
        if (loops.empty())
          ImGui::SetTooltip("No loops yet. Drag with the right mouse button across the ruler to make one");
        else if (active >= 0)
          ImGui::SetTooltip("Looping \"%s\". Click to play straight through (L), hold for the list",
                            loops[static_cast<size_t>(active)].name.c_str());
        else
          ImGui::SetTooltip("Play the selected loop over and over (L), or hold for the list");
      }

      // Standing on top of the key rather than floating anywhere, and pinned: this is a list you
      // point at, not a window you arrange.
      ImGui::SetNextWindowPos(ImVec2(loopMin.x, loopMin.y - 6.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Border, theme::Line());
      ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
      const bool pickOpen = ImGui::BeginPopup("##looppick", ImGuiWindowFlags_NoMove
                                                              | ImGuiWindowFlags_AlwaysAutoResize
                                                              | ImGuiWindowFlags_NoSavedSettings);
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();

      if (pickOpen)
      {
        const int running = player.GetActiveLoop();
        for (size_t i = 0; i < loops.size(); i++)
        {
          const Loop& loop = loops[i];
          char label[128];
          std::snprintf(label, sizeof(label), "%s##loop%zu", loop.name.c_str(), i);

          if (ImGui::Selectable(label, static_cast<int>(i) == running, 0, ImVec2(190.0f, 0.0f)))
          {
            player.SetActiveLoop(static_cast<int>(i));
            player.SetPositionSeconds(loop.startSeconds);
            mSelectedLoop = static_cast<int>(i);
            ImGui::CloseCurrentPopup();
          }

          // How long it is, out at the right: two loops with the same name are told apart by it,
          // and it is the one number you want when choosing which passage to work.
          char length[32];
          std::snprintf(length, sizeof(length), "%.1f s", loop.endSeconds - loop.startSeconds);
          ImGui::SameLine(150.0f);
          ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint());
          ImGui::TextUnformatted(length);
          ImGui::PopStyleColor();
        }

        if (running >= 0)
        {
          ImGui::Separator();
          if (ImGui::Selectable("Play straight through", false, 0, ImVec2(190.0f, 0.0f)))
          {
            player.SetActiveLoop(-1);
            ImGui::CloseCurrentPopup();
          }
        }

        ImGui::EndPopup();
      }

      left += keySize.x + 6.0f;
    }

    // --- how fast, and in what key ---
    //
    // Two faders behind one key, because they are set at the start of a passage and then left: on
    // the bar the whole time they were two labelled sliders and three hundred pixels, for something
    // touched once. Lit whenever either is away from where it started, so a song playing slow or in
    // another key never looks like a song playing normally.
    {
      const float currentSpeed = player.GetSpeed();
      const float currentPitch = player.GetSemitones();
      const bool altered = std::fabs(currentSpeed - 1.0f) > 0.005f || std::fabs(currentPitch) > 0.01f;

      placeAt(left, keySize.y);
      if (theme::TransportKey("##speedpitch", theme::Icon::Pitch, keySize, false, altered))
        ImGui::OpenPopup("##speedpitchpop");

      const ImVec2 keyMin = ImGui::GetItemRectMin();
      if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##speedpitchpop"))
      {
        if (altered)
          ImGui::SetTooltip("%.0f%% speed, %+.0f semitones. Click to change", currentSpeed * 100.0f, currentPitch);
        else
          ImGui::SetTooltip("Speed and pitch, each without the other");
      }

      ImGui::SetNextWindowPos(ImVec2(keyMin.x + keySize.x * 0.5f, keyMin.y - 6.0f), ImGuiCond_Always,
                              ImVec2(0.5f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Border, theme::Line());
      ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
      const bool spOpen = ImGui::BeginPopup("##speedpitchpop", ImGuiWindowFlags_NoMove
                                                                 | ImGuiWindowFlags_AlwaysAutoResize
                                                                 | ImGuiWindowFlags_NoSavedSettings);
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();

      if (spOpen)
      {
        constexpr float kFader = 128.0f;

        ImGui::BeginGroup();
        theme::Label("Speed");
        float percent = currentSpeed * 100.0f;
        // Catching at 100 within two points: a fader you can put back exactly without watching it.
        if (theme::SlimSliderVertical("##speedfader", &percent, kMinPlaybackSpeed * 100.0f,
                                      kMaxPlaybackSpeed * 100.0f, 100.0f, kFader, "%.0f%%", 100.0f, 2.0f))
          player.SetSpeed(percent / 100.0f);
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 30.0f);

        ImGui::BeginGroup();
        theme::Label("Pitch");
        float semitones = currentPitch;
        // Half a semitone either side of nothing, which is as wide as it can be without swallowing
        // the first step up and the first step down.
        if (theme::SlimSliderVertical("##pitchfader", &semitones, -12.0f, 12.0f, 0.0f, kFader, "%+.0f st", 0.0f, 0.5f))
          player.SetSemitones(std::round(semitones));
        ImGui::EndGroup();

        // The round numbers, kept from the right-click menu these two used to have. A quarter speed
        // is a thing you ask for by name, not by dragging until the number looks right.
        //
        // Buttons rather than selectables: four percentages in a row under two faders read as a
        // caption of what the faders are set to, and nobody presses a caption.
        ImGui::Spacing();
        for (const float preset : {1.0f, 0.75f, 0.5f, 0.25f})
        {
          char label[16];
          std::snprintf(label, sizeof(label), "%.0f%%", preset * 100.0f);
          if (preset != 1.0f)
            ImGui::SameLine(0.0f, 4.0f);

          // The theme's button has no fill at rest, which on this black is no edge at all - so each
          // gets one here. The one the song is actually playing at takes the accent instead.
          const bool current = std::fabs(currentSpeed - preset) < 0.005f;
          ImGui::PushStyleColor(ImGuiCol_Button, current ? theme::AccentDim() : ImVec4(1, 1, 1, 0.07f));
          ImGui::PushStyleColor(ImGuiCol_Text, current ? theme::Accent() : theme::TextDim());
          if (ImGui::Button(label, ImVec2(34.0f, 0.0f)))
            player.SetSpeed(preset);
          ImGui::PopStyleColor(2);
        }

        ImGui::EndPopup();
      }

      left += keySize.x + 14.0f;
    }

    placeAt(left, keySize.y);
    if (theme::TransportKey("##showgrid", theme::Icon::Grid, keySize, false, mShowGrid))
      mShowGrid = !mShowGrid;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(mTempo.valid ? (mShowGrid ? "Bars and beats drawn through the tracks. Click for a clock instead"
                                                  : "The ruler is counting seconds. Click for bars and beats")
                                     : "Find the song's tempo first, and the grid has something to draw");
    left += keySize.x + 4.0f;

    placeAt(left, keySize.y);
    if (theme::TransportKey("##snapgrid", theme::Icon::Snap, keySize, false, mSnapToGrid))
      mSnapToGrid = !mSnapToGrid;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(mSnapToGrid ? "Loop edges land on the nearest beat" : "Loop edges land where you let go");
    left += keySize.x + 4.0f;

    placeAt(left, keySize.y);
    if (theme::TransportKey("##followhead", theme::Icon::Follow, keySize, false, mFollowPlayhead))
      mFollowPlayhead = !mFollowPlayhead;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(mFollowPlayhead ? "The view keeps up with the playhead (F)"
                                        : "The view stays where you put it (F)");
  }

  // --- the scrub bar, under the keys ---
  //
  // Where you are in the song, and how you get somewhere else. The timeline above shows whatever
  // is zoomed into; this is always the whole song, so there is one place that always answers "how
  // far in am I" no matter what the view is doing.
  {
    const float scrubY = barOrigin.y + bayHeight + 12.0f;
    constexpr float kScrubHeight = 16.0f;
    constexpr float kTrack = 4.0f;

    // A little wider than the keys above it and centred on the same axis, so the two read as one
    // control. Stretched across the window it stopped belonging to the transport and became a
    // border along the bottom of the screen.
    const float scrubWidth = clusterWidth + 96.0f;
    const float scrubX = barOrigin.x + (barWidth - scrubWidth) * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(scrubX, scrubY));
    ImGui::InvisibleButton("##scrub", ImVec2(scrubWidth, kScrubHeight));
    const bool scrubbing = ImGui::IsItemActive();
    const bool overScrub = ImGui::IsItemHovered() || scrubbing;

    if (scrubbing && duration > 0.0)
    {
      const float t = std::clamp((ImGui::GetIO().MousePos.x - scrubX) / scrubWidth, 0.0f, 1.0f);
      player.SetPositionSeconds(static_cast<double>(t) * duration);
      mFollowPlayhead = true; // dragging here means you want to watch where you landed
    }

    const float midY = scrubY + kScrubHeight * 0.5f;
    const float played =
      (duration > 0.0) ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) * scrubWidth : 0.0f;

    draw->AddRectFilled(ImVec2(scrubX, midY - kTrack * 0.5f), ImVec2(scrubX + scrubWidth, midY + kTrack * 0.5f),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)), kTrack * 0.5f);
    draw->AddRectFilled(ImVec2(scrubX, midY - kTrack * 0.5f), ImVec2(scrubX + played, midY + kTrack * 0.5f),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, overScrub ? 0.85f : 0.55f)), kTrack * 0.5f);

    // Grey until you reach for it. A permanently lit handle spends the accent on a thing that is
    // just sitting there; lighting it under the pointer spends it on the moment it means
    // something, which is the whole of how colour is used here.
    draw->AddCircleFilled(ImVec2(scrubX + played, midY), overScrub ? 7.0f : 5.0f,
                          ImGui::GetColorU32(overScrub ? theme::Accent() : ImVec4(1, 1, 1, 0.72f)), 24);

    // The same bubble the channel faders use, over the handle - so the time you are dragging to is
    // where your eye already is rather than back at the end of the bar.
    if (overScrub)
    {
      const std::string bubble = FormatTime(position);
      const ImVec2 extent = ImGui::CalcTextSize(bubble.c_str());
      const ImVec2 padding(8.0f, 3.0f);
      const ImVec2 from(scrubX + played - (extent.x + padding.x * 2.0f) * 0.5f,
                        midY - 10.0f - (extent.y + padding.y * 2.0f));
      const ImVec2 to(from.x + extent.x + padding.x * 2.0f, from.y + extent.y + padding.y * 2.0f);

      ImDrawList* front = ImGui::GetForegroundDrawList();
      front->AddRectFilled(from, to, ImGui::GetColorU32(theme::Control()), (to.y - from.y) * 0.5f);
      front->AddText(ImVec2(from.x + padding.x, from.y + padding.y), ImGui::GetColorU32(theme::Base()),
                     bubble.c_str());
    }

    // Where you are at the near end of the bar, where the song ends at the far one - so the two
    // numbers bracket the distance between them rather than sitting at opposite ends of the window.
    const std::string now = FormatTime(position);
    const std::string total = FormatTime(duration);

    theme::PushHeading(1.15f);
    const ImVec2 nowSize = ImGui::CalcTextSize(now.c_str());
    ImGui::SetCursorScreenPos(ImVec2(scrubX - nowSize.x - 14.0f, midY - nowSize.y * 0.5f));
    ImGui::TextUnformatted(now.c_str());
    theme::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(scrubX + scrubWidth + 14.0f, midY - ImGui::GetFontSize() * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint());
    ImGui::TextUnformatted(total.c_str());
    ImGui::PopStyleColor();
  }

  // Back onto the flow, and an item to say how far the hand-placed rows reached. Without one, ImGui
  // has no way to know the content grew - moving the cursor is not the same as filling the space,
  // and it says so.
  ImGui::SetCursorScreenPos(ImVec2(barOrigin.x, barOrigin.y + bayHeight + 30.0f));
  ImGui::Dummy(ImVec2(barWidth, 0.0f));
}

void MainWindow::DrawTrackEqPopup()
{
  if (mEqTrackId == 0)
    return;

  // It belongs to a track on the timeline, so it lives as long as the timeline is on screen and
  // no longer. Left open it followed you into the tuner, floating over a view with no tracks in
  // it and no way to tell what it was the EQ of.
  if (mView != View::Player)
  {
    mEqTrackId = 0;
    return;
  }

  Player& player = mEngine.GetPlayer();
  auto tracks = player.GetTracks();

  Track* found = nullptr;
  for (auto& candidate : tracks)
    if (candidate.id == mEqTrackId)
      found = &candidate;

  if (found == nullptr)
  {
    mEqTrackId = 0; // the track was closed while its EQ was open
    return;
  }

  Track track = *found;
  const std::string title = (track.name.empty() ? std::string("Track") : track.name) + " EQ###trackeq";

  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_Appearing);
  // An outline, so it reads as a thing lying over the timeline rather than a black rectangle with
  // controls in it - every background in the app is the same black, and without an edge a floating
  // window has none. No collapse: it is one panel about one track, and folding it to a title bar
  // leaves a stub that has to be found again. Close is the only thing it needs.
  ImGui::PushStyleColor(ImGuiCol_Border, theme::Line());
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  if (visible)
  {
    bool changed = false;

    if (theme::Check("On", &track.eqEnabled))
      changed = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Off leaves the track exactly as it was recorded");

    ImGui::SameLine(0, 16);
    if (ImGui::Button("Flat", ImVec2(70, 0)))
    {
      for (auto& band : track.eq.bands)
        band.gainDb = 0.0f;
      changed = true;
    }

    ImGui::SameLine(0, 16);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("Drag a point. Wheel over one sets its Q");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // No spectrum behind it: the analyser is fed by the amp's input, and drawing your playing
    // behind a backing track's EQ would be showing the wrong signal.
    const float plotHeight = std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.4f);
    if (DrawParametricEq(track.eq, ImGui::GetContentRegionAvail().x, plotHeight, false))
    {
      // Dragging a band is asking to hear it, so it switches the EQ on rather than leaving you
      // wondering why nothing changed.
      track.eqEnabled = true;
      changed = true;
    }

    ImGui::Spacing();
    for (size_t i = 0; i < kEqBandCount; i++)
    {
      ImGui::PushID(static_cast<int>(i));
      if (i > 0)
        ImGui::SameLine();
      if (theme::Check("##band", &track.eq.bands[i].enabled))
        changed = true;
      ImGui::SameLine(0, 3);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::Text("%d: %.0f Hz", static_cast<int>(i) + 1, track.eq.bands[i].hz);
      ImGui::PopStyleColor();
      ImGui::PopID();
    }

    if (changed)
      player.SetTrack(track);
  }
  ImGui::End();

  if (!open)
    mEqTrackId = 0;
}

void MainWindow::StartNewProject()
{
  Player& player = mEngine.GetPlayer();
  player.Clear();

  mTrackNotes.clear();
  RebuildSynthScore();

  mSelectedLoop = -1;
  mSelectedTrackId = 0;
  mRenamingTrackId = 0;
  mEqTrackId = 0;
  mPlayerError.clear();

  mTempo = TempoEstimate();
  mViewStart = 0.0;
  mViewDuration = 0.0;
  mLaneHeight = 0.0f;
  mLaneScroll = 0.0f;

  mProjectFile.clear();
  mProjectName.clear();
  mProjectDirty = false;
}

void MainWindow::DrawTempoPanel(float width)
{
  // The grid, on the channel that plays it. Before there is one this is a button that goes and
  // finds it; afterwards the same place reads the tempo back and opens the corrections. One
  // control that changes what it says, rather than a reading in one corner and the button that
  // produces it in another.
  Player& tempoPlayer = mEngine.GetPlayer();
  const auto tempoTracks = tempoPlayer.GetTracks();

  if (!mTempo.valid)
  {
    ImGui::BeginDisabled(tempoTracks.empty() || mTempoAnalyser.IsRunning());
    const bool asked = ImGui::Button("Detect tempo", ImVec2(width, 0));
    ImGui::EndDisabled();

    if (asked)
      mTempoAnalyser.Start(tempoTracks.front().file);

    if (mTempoAnalyser.IsRunning())
    {
      ImGui::SameLine(0, 8);
      theme::Spinner(ImGui::GetFrameHeight() * 0.8f);
    }
    else if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip(tempoTracks.empty() ? "Import a song first"
                                            : "Listens through the first track for its beats. Run it on a drum "
                                              "stem and it gets a lot better");
    }
    return;
  }

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetFrameHeight();
  ImGui::InvisibleButton("##tempoopen", ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked())
    ImGui::OpenPopup("##tempopop");

  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (hovered)
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f)), 5.0f);

  char reading[64];
  std::snprintf(reading, sizeof(reading), "%.2f BPM", mTempo.bpm);

  // A low confidence turns the number itself amber rather than adding a "42% sure" beside it. The
  // number is what you would check, so the doubt belongs on the number.
  const bool unsure = mTempo.confidence > 0.0f && mTempo.confidence < 0.35f;
  const ImU32 colour = ImGui::GetColorU32(unsure ? theme::Warning() : theme::Text());

  const float lineHeight = ImGui::GetTextLineHeight();
  draw->AddText(ImVec2(origin.x + 8.0f, origin.y + (height - lineHeight) * 0.5f), colour, reading);

  {
    char bar[16];
    std::snprintf(bar, sizeof(bar), "%d/4", std::max(1, mGridBeatsPerBar));
    const float barWidth = ImGui::CalcTextSize(bar).x;
    draw->AddText(ImVec2(origin.x + width - barWidth - 10.0f, origin.y + (height - lineHeight) * 0.5f),
                  ImGui::GetColorU32(theme::TextFaint()), bar);
  }

  if (hovered && !ImGui::IsPopupOpen("##tempopop"))
    ImGui::SetTooltip("The song's grid. Click to correct it");

  if (ImGui::BeginPopup("##tempopop"))
  {
    Player& player = mEngine.GetPlayer();
    auto currentTracks = player.GetTracks();

    theme::SectionLabel("GRID");

    const bool busy = mTempoAnalyser.IsRunning() || mDeepBeats.IsRunning();

    // The track it listens to: the one you have selected, or the first if none is. Which stem it
    // hears matters a great deal to this tracker - a drum stem is far easier than a mix - so
    // "detect" following the selection is the difference between it being usable and not.
    std::shared_ptr<AudioFile> subject;
    std::string subjectName;
    for (const auto& track : currentTracks)
      if (track.id == mSelectedTrackId && track.file)
      {
        subject = track.file;
        subjectName = track.name.empty() ? track.file->name : track.name;
      }
    if (!subject && !currentTracks.empty())
    {
      subject = currentTracks.front().file;
      subjectName = currentTracks.front().name.empty() ? currentTracks.front().file->name
                                                       : currentTracks.front().name;
    }

    ImGui::BeginDisabled(!subject || busy);
    if (ImGui::Button("Detect again", ImVec2(150, 0)))
      mTempoAnalyser.Start(subject);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Listens to %s. Select another channel to use that one instead - a drum stem "
                        "is much easier for this than a mix",
                        subjectName.empty() ? "the first channel" : subjectName.c_str());

    // --- the deeper pass ---
    //
    // The fast tracker places beats within a few milliseconds but often counts the wrong pulse -
    // the eighths on a slow song, the half notes on a fast one. Choosing the metrical level is a
    // judgement about how the music is heard, and no amount of tuning the signal analysis settles
    // it; this is a model that was trained on that judgement. It costs a Python install and a few
    // seconds, which is why it is the second button rather than the first.
    ImGui::SameLine();
    ImGui::BeginDisabled(currentTracks.empty() || busy);
    if (ImGui::Button("Listen harder", ImVec2(150, 0)))
      StartDeepBeats();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Beat This!, a model trained on where people hear the beat. Much better at "
                        "picking the right pulse and at music without drums.\n\nThis one always "
                        "hears the whole mix, whichever channel is selected: it was trained on "
                        "records, and on a lone stem it reports no beats wherever that instrument "
                        "is not playing.\n\nNeeds Python, and takes a few seconds");

    if (busy)
    {
      ImGui::SameLine();
      theme::Spinner(ImGui::GetFrameHeight());
    }

    if (mDeepBeats.IsRunning())
      theme::Hint(mDeepBeats.GetStatus().c_str());
    else if (!mDeepBeats.GetError().empty())
    {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
      ImGui::TextWrapped("%s", mDeepBeats.GetError().c_str());
      ImGui::PopStyleColor();
    }

    theme::LabelFor("Tempo", 90.0f);
    ImGui::SetNextItemWidth(100);
    {
      // Correcting the tempo by hand stretches the tracked grid rather than replacing it: a song
      // that was followed correctly but read as half speed should stay followed.
      const float before = mTempo.bpm;
      if (ImGui::DragFloat("##gridbpm", &mTempo.bpm, 0.1f, 20.0f, 300.0f, "%.2f"))
      {
        if (before > 0.0f && mTempo.bpm > 0.0f)
          mTempo.Scale(static_cast<double>(before) / static_cast<double>(mTempo.bpm));
        mTempo.valid = true;
      }
    }

    // Nudging where the grid starts is the correction that gets used most: the tempo is usually
    // right and the downbeat usually is not.
    theme::LabelFor("Offset", 90.0f);
    ImGui::SetNextItemWidth(100);
    float offset = static_cast<float>(mTempo.firstBeatSeconds);
    if (ImGui::DragFloat("##gridoffset", &offset, 0.002f, -5.0f, 30.0f, "%.3f s"))
    {
      mTempo.Shift(static_cast<double>(offset) - mTempo.firstBeatSeconds);
      mTempo.firstBeatSeconds = offset;
      mTempo.valid = true;
    }

    theme::LabelFor("Bar", 90.0f);
    ImGui::SetNextItemWidth(100);
    ImGui::DragInt("##gridbeats", &mGridBeatsPerBar, 0.1f, 1, 12, "%d beats");

    if (mTempo.valid && mTempo.confidence > 0.0f)
    {
      char note[160];
      if (mTempo.Drifts())
        std::snprintf(note, sizeof(note),
                      "%.0f%% sure. This song's tempo moves between %.0f and %.0f - the grid was tracked "
                      "through it beat by beat, so it stays lined up to the end.",
                      mTempo.confidence * 100.0f, mTempo.lowestBpm, mTempo.highestBpm);
      else
        std::snprintf(note, sizeof(note),
                      "%.0f%% sure. Beat tracking is often half or double the real tempo - the three "
                      "buttons on the click's channel fix that without moving a beat.",
                      mTempo.confidence * 100.0f);
      ImGui::Dummy(ImVec2(0.0f, 4.0f));
      ImGui::PushTextWrapPos(300.0f);
      theme::Hint(note);
      ImGui::PopTextWrapPos();
    }

    ImGui::EndPopup();
  }
}

void MainWindow::DrawProjectBar()
{
  Player& player = mEngine.GetPlayer();

  // --- the project's name, and everything you can do to the project behind it ---
  //
  // Six buttons became a caret. Opening, saving and separating are things done once or twice in a
  // session, and a row of them across the top gave them the same weight as the two controls that
  // get touched while playing. Every document-shaped application puts its file actions behind the
  // document's own name, and Ctrl+S and Ctrl+O still work without going near this.
  const std::string name = (mProjectName.empty() ? std::string("Not saved") : mProjectName) + "###project";
  const bool openMenu = theme::MenuButton(name.c_str(), 1.2f);

  if (openMenu)
    ImGui::OpenPopup("##projectmenu");
  if (ImGui::IsItemHovered() && !mProjectFile.empty())
    ImGui::SetTooltip("%s", mProjectFile.string().c_str());

  if (ImGui::BeginPopup("##projectmenu"))
  {
    const bool hasTracks = !player.GetTracks().empty();

    // The order a session runs in: start something, or come back to something, then keep it, then
    // put music in it.
    if (ImGui::MenuItem("New"))
    {
      if (mProjectDirty && hasTracks)
        mConfirmNewOpen = true; // there is work here that starting again would lose
      else
        StartNewProject();
    }

    if (ImGui::MenuItem("Open project...", "Ctrl+O"))
      BrowseAndOpenProject();

    // The ones you have had open, since coming back to yesterday's work is the common case and
    // finding it through a file dialog every time is not.
    if (ImGui::BeginMenu("Open recent", !mConfig.recentProjects.empty()))
    {
      std::string missing;
      for (const auto& path : mConfig.recentProjects)
      {
        const std::filesystem::path file(path);
        std::error_code ec;
        const bool there = std::filesystem::exists(file, ec);

        ImGui::BeginDisabled(!there);
        // The project's own name, with the folder it is in underneath as the shortcut column -
        // two projects called "Take 1" are told apart by where they live.
        if (ImGui::MenuItem(file.stem().string().c_str(), file.parent_path().filename().string().c_str()))
          OpenProject(file);
        ImGui::EndDisabled();

        if (!there && missing.empty())
          missing = path;
      }

      // Nothing is dropped from the list behind your back; forgetting is asked for.
      if (!missing.empty())
      {
        ImGui::Separator();
        if (ImGui::MenuItem("Forget the ones that are gone"))
        {
          auto& recent = mConfig.recentProjects;
          recent.erase(std::remove_if(recent.begin(), recent.end(),
                                      [](const std::string& path)
                                      {
                                        std::error_code ec;
                                        return !std::filesystem::exists(std::filesystem::path(path), ec);
                                      }),
                       recent.end());
          mConfig.Save();
        }
      }
      ImGui::EndMenu();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Save", "Ctrl+S", false, hasTracks))
    {
      if (mProjectFile.empty())
      {
        std::snprintf(mProjectNameInput, sizeof(mProjectNameInput), "%s",
                      mProjectName.empty() ? "New project" : mProjectName.c_str());
        mSaveProjectDialogOpen = true;
      }
      else
      {
        SaveProjectTo(mProjectFile.parent_path(), mProjectName);
      }
    }

    // Greyed until there is a project to save a copy of - "save as" with nothing saved yet is
    // just "save", and offering both says they are different things.
    if (ImGui::MenuItem("Save as...", nullptr, false, hasTracks && !mProjectFile.empty()))
    {
      std::snprintf(mProjectNameInput, sizeof(mProjectNameInput), "%s",
                    mProjectName.empty() ? "New project" : mProjectName.c_str());
      mSaveProjectDialogOpen = true;
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Import song..."))
      AddPlayerTrack();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Bring in a wav, mp3 or flac. Several of them play locked together");

    ImGui::EndPopup();
  }

  // A run sent to the background is reported in the lane it was started from, which is where the
  // offer to start it was made and where the result will arrive.

  // --- what the timeline shows, out at the right ---
  const float viewButton = ImGui::GetFrameHeight();
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - viewButton - 130.0f);

  if (theme::IconButton("##viewmenu", theme::Icon::Sliders, viewButton, theme::IconStyle::Bare))
    ImGui::OpenPopup("##viewpopup");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("What the timeline shows");

  // The same edge the track EQ has: both are panels lying over the timeline, and against one black
  // background an outline is the only thing that says where one stops.
  ImGui::PushStyleColor(ImGuiCol_Border, theme::Line());
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
  const bool viewOpen = ImGui::BeginPopup("##viewpopup");
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (viewOpen)
  {
    theme::SectionLabel("ZOOM");
    ImGui::BeginDisabled(player.DurationSeconds() <= 0.0);
    if (ImGui::Button("Whole song", ImVec2(150, 0)))
    {
      mViewStart = 0.0;
      mViewDuration = 0.0;
      mLaneHeight = 0.0f;
      mLaneScroll = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("To the loop", ImVec2(150, 0)))
    {
      const auto currentLoops = player.GetLoops();
      const int active = player.GetActiveLoop();
      if (active >= 0 && static_cast<size_t>(active) < currentLoops.size())
      {
        const Loop& loop = currentLoops[static_cast<size_t>(active)];
        const double margin = std::max(0.25, (loop.endSeconds - loop.startSeconds) * 0.1);
        mViewStart = std::max(0.0, loop.startSeconds - margin);
        mViewDuration = (loop.endSeconds - loop.startSeconds) + margin * 2.0;
      }
    }
    ImGui::EndDisabled();

    theme::LabelFor("Row height", 110.0f);
    if (theme::IconButton("##rowsdown", theme::Icon::Minus, ImGui::GetFrameHeight(), theme::IconStyle::Raised))
    {
      const float current = (mLaneHeight > 0.0f) ? mLaneHeight : 90.0f;
      mLaneHeight = std::clamp(current / 1.25f, 32.0f, 400.0f);
    }
    ImGui::SameLine(0, 4);
    if (theme::IconButton("##rowsup", theme::Icon::Plus, ImGui::GetFrameHeight(), theme::IconStyle::Raised))
    {
      const float current = (mLaneHeight > 0.0f) ? mLaneHeight : 90.0f;
      mLaneHeight = std::clamp(current * 1.25f, 32.0f, 400.0f);
    }

    // The grid, the follow and the snap are on the transport now, where they are one press away
    // instead of two and where their state is visible without opening anything.

    theme::SectionLabel("KEYS");
    theme::Hint("Space play   Home start   0 stop\n"
                "Left / Right  five seconds either way\n"
                "Shift+Left / Right  previous / next loop\n"
                "L loop   C click   F follow   G / H zoom\n"
                "M / S  mute / solo the selected track\n"
                "Ctrl+S save   Ctrl+O open");

    ImGui::EndPopup();
  }
}

void MainWindow::DrawSaveProjectPopup()
{
  if (!mSaveProjectDialogOpen)
    return;

  if (!ImGui::IsPopupOpen("Save project"))
    ImGui::OpenPopup("Save project");

  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Save project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    mSaveProjectDialogOpen = false;
    return;
  }

  if (mProjectParent.empty())
    mProjectParent = ProjectFile::DefaultFolder();

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("A project is a folder. The audio is copied into it, so it still opens after "
                     "the stems it was built from have been moved or cleared out.");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::SetNextItemWidth(-1);
  const bool submitted =
    ImGui::InputTextWithHint("##projectname", "Project name", mProjectNameInput, sizeof(mProjectNameInput),
                             ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere(-1);

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Into %s", (mProjectParent / mProjectNameInput).string().c_str());
  ImGui::PopStyleColor();

  ImGui::Spacing();
  if (ImGui::Button("Choose folder...", ImVec2(160, 0)))
  {
    HWND hwnd = glfwGetWin32Window(mWindow);
    auto picked = BrowseForFolder(hwnd, L"Where to keep this project", mProjectParent);
    if (picked.has_value())
      mProjectParent = *picked;
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  const bool named = mProjectNameInput[0] != '\0';
  ImGui::BeginDisabled(!named);
  const bool save = ImGui::Button("Save", ImVec2(140, 0)) || (submitted && named);
  ImGui::EndDisabled();

  if (save)
  {
    SaveProjectTo(mProjectParent / mProjectNameInput, mProjectNameInput);
    mSaveProjectDialogOpen = false;
    ImGui::CloseCurrentPopup();
  }

  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(140, 0)))
  {
    mSaveProjectDialogOpen = false;
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void MainWindow::SaveProjectTo(const std::filesystem::path& folder, const std::string& name)
{
  Player& player = mEngine.GetPlayer();
  const auto tracks = player.GetTracks();

  ProjectFile project;
  project.name = name;

  std::vector<std::filesystem::path> sources;
  for (const auto& track : tracks)
  {
    if (!track.file)
      continue;
    ProjectFile::TrackEntry entry;
    entry.name = track.name;
    entry.gainDb = track.gainDb;
    entry.muted = track.muted;
    entry.soloed = track.soloed;
    entry.eqEnabled = track.eqEnabled;
    entry.eq = track.eq;

    const auto notes = mTrackNotes.find(track.id);
    if (notes != mTrackNotes.end())
      entry.notes = notes->second;

    project.tracks.push_back(std::move(entry));
    sources.push_back(track.file->path);
  }

  for (const auto& loop : player.GetLoops())
    project.loops.push_back({loop.name, loop.startSeconds, loop.endSeconds});
  project.activeLoop = player.GetActiveLoop();

  project.gainDb = player.GetGainDb();
  project.speed = player.GetSpeed();
  project.semitones = player.GetSemitones();
  project.positionSeconds = player.GetPositionSeconds();

  project.tempoValid = mTempo.valid;
  project.bpm = mTempo.bpm;
  project.firstBeatSeconds = mTempo.firstBeatSeconds;
  project.beats = mTempo.beats;
  project.downbeatOffset = mTempo.downbeatOffset;
  project.lowestBpm = mTempo.lowestBpm;
  project.highestBpm = mTempo.highestBpm;
  project.confidence = mTempo.confidence;
  project.beatsPerBar = mGridBeatsPerBar;
  project.showGrid = mShowGrid;
  project.snapToGrid = mSnapToGrid;

  project.viewStart = mViewStart;
  project.viewDuration = mViewDuration;
  project.followPlayhead = mFollowPlayhead;

  project.metronome = mMetronome;
  project.clickArmed = mClickArmed;

  std::string error;
  if (!project.Save(folder, sources, error))
  {
    mPlayerError = error;
    return;
  }

  // The tracks now have copies inside the project, and that is what they play from here on: a
  // second save would otherwise copy the originals in all over again under a new name.
  size_t index = 0;
  for (const auto& track : tracks)
  {
    if (!track.file || index >= project.tracks.size())
      continue;
    track.file->path = folder / project.tracks[index].relativePath;
    index++;
  }

  mProjectFile = ProjectFile::FileFor(folder, name);
  mProjectName = name;
  mProjectParent = folder.parent_path();
  mProjectDirty = false;
  mPlayerError.clear();
  RememberProject(mProjectFile);
}

/// Puts a project at the top of the recent list, without letting it appear twice.
void MainWindow::RememberProject(const std::filesystem::path& file)
{
  if (file.empty())
    return;

  const std::string path = file.string();
  auto& recent = mConfig.recentProjects;
  recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
  recent.insert(recent.begin(), path);
  if (recent.size() > 10)
    recent.resize(10);
  mConfig.Save();
}

/// Asks for a project file and opens it. Offered in two places - the project menu and the empty
/// timeline - so it lives in one.
void MainWindow::BrowseAndOpenProject()
{
  HWND hwnd = glfwGetWin32Window(mWindow);
  // Both extensions, so a project made before the app was named still shows up in the dialog
  // rather than being filtered out of its own folder.
  auto picked = BrowseForFile(hwnd, L"Open a project", L"Woodshed project", L"shed;*.namproj");
  if (picked.has_value())
    OpenProject(*picked);
}

void MainWindow::OpenProject(const std::filesystem::path& file)
{
  ProjectFile project;
  std::string error;
  if (!ProjectFile::Load(file, project, error))
  {
    mPlayerError = error;
    return;
  }

  Player& player = mEngine.GetPlayer();
  player.Clear();
  mSelectedLoop = -1;
  mSelectedTrackId = 0;
  mRenamingTrackId = 0;
  mEqTrackId = 0;
  mTrackNotes.clear();
  mPlayerError.clear();

  std::string missing;
  for (const auto& entry : project.tracks)
  {
    const std::filesystem::path path = project.folder / entry.relativePath;
    const int id = AddPlayerTrackFromPath(path);
    if (id < 0)
    {
      // One track that will not load is not a reason to lose the rest of the project.
      missing += (missing.empty() ? "" : ", ") + std::filesystem::path(entry.relativePath).filename().string();
      continue;
    }

    for (auto& track : player.GetTracks())
    {
      if (track.id != id)
        continue;
      Track updated = track;
      updated.name = entry.name.empty() ? track.name : entry.name;
      updated.gainDb = entry.gainDb;
      updated.muted = entry.muted;
      updated.soloed = entry.soloed;
      updated.eqEnabled = entry.eqEnabled;
      updated.eq = entry.eq;
      player.SetTrack(updated);
    }

    // The analysis was done once and stored; there is no reason to spend the seconds again.
    if (!entry.notes.notes.empty() || !entry.notes.chords.empty())
      mTrackNotes[id] = entry.notes;
  }

  std::vector<Loop> loops;
  for (const auto& entry : project.loops)
    loops.push_back({entry.name, entry.startSeconds, entry.endSeconds});
  player.SetLoops(loops);
  player.SetActiveLoop(project.activeLoop);

  player.SetGainDb(project.gainDb);
  player.SetSpeed(project.speed);
  player.SetSemitones(project.semitones);
  player.SetPositionSeconds(project.positionSeconds);

  mTempo.valid = project.tempoValid;
  mTempo.bpm = project.bpm;
  mTempo.firstBeatSeconds = project.firstBeatSeconds;
  mTempo.beats = project.beats;
  mTempo.downbeatOffset = project.downbeatOffset;
  mTempo.lowestBpm = project.lowestBpm;
  mTempo.highestBpm = project.highestBpm;
  mTempo.confidence = project.confidence;
  mGridBeatsPerBar = project.beatsPerBar;
  mShowGrid = project.showGrid;
  mSnapToGrid = project.snapToGrid;

  mViewStart = project.viewStart;
  mViewDuration = project.viewDuration;
  mFollowPlayhead = project.followPlayhead;

  RebuildSynthScore(); // the project's tracks brought their playback settings with them

  mMetronome = project.metronome;
  mClickArmed = project.clickArmed;
  mMetronomeRunning = false; // armed, not running: it starts when the song does
  ApplyMetronome();

  mProjectFile = file;
  mProjectName = project.name;
  mProjectParent = project.folder.parent_path();
  mProjectDirty = false; // what is on screen is exactly what is on disk
  RememberProject(file);

  if (!missing.empty())
    mPlayerError = "Could not open: " + missing;
}

/// \brief Hands the deep tracker the whole mix, whatever the project has been taken apart into.
///
/// The model was trained on records. A drum stem is off the distribution it learned, and it shows:
/// on a song whose drums do not come in for over a minute it correctly reports no beats at all for
/// that minute, which is right about the stem and useless as a grid for the song. So when the
/// project is several stems they are summed back into the record they came from, written out once,
/// and that is what it listens to.
void MainWindow::StartDeepBeats()
{
  const auto tracks = mEngine.GetPlayer().GetTracks();
  if (tracks.empty())
    return;

  DeepBeatTracker::Options options;
  options.command = TranscribePython();
  options.useDbn = mDeepBeatsDbn;

  const std::filesystem::path workFolder = AppConfig::GetConfigFolder() / "beats";

  // One track is already the mix.
  if (tracks.size() == 1)
  {
    if (tracks.front().file && !tracks.front().file->path.empty())
    {
      mDeepBeats.Start(tracks.front().file->path, workFolder, options);
      return;
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(workFolder, ec);

  // Summed at the levels they were separated at, not at the levels they are being played at: what
  // the model wants is the record, and a stem you happen to have muted is still part of it.
  AudioFile mix;
  size_t longest = 0;
  for (const auto& track : tracks)
  {
    if (!track.file)
      continue;
    longest = std::max(longest, track.file->FrameCount());
    mix.sampleRate = track.file->sampleRate;
  }

  if (longest == 0)
    return;

  mix.samples.assign(longest * 2, 0.0f);
  for (const auto& track : tracks)
  {
    if (!track.file)
      continue;
    const size_t frames = track.file->FrameCount();
    for (size_t i = 0; i < frames * 2; i++)
      mix.samples[i] += track.file->samples[i];
  }

  // Summing stems back together overshoots wherever they were mastered into each other. Scaled
  // rather than clipped: what a limiter would do to the transients is exactly what the onsets are.
  float peak = 0.0f;
  for (const float sample : mix.samples)
    peak = std::max(peak, std::fabs(sample));
  if (peak > 1.0f)
    for (float& sample : mix.samples)
      sample /= peak;

  const std::filesystem::path mixPath = workFolder / "mix.wav";
  std::string error;
  if (!AudioFile::Write(mixPath, mix, error))
  {
    mPlayerError = error;
    return;
  }

  mDeepBeats.Start(mixPath, workFolder, options);
}

std::filesystem::path MainWindow::ManagedPythonPath()
{
  return AppConfig::GetConfigFolder() / "python311" / "Scripts" / "python.exe";
}

std::string MainWindow::TranscribePython() const
{
  if (!mConfig.transcribeCommand.empty())
    return mConfig.transcribeCommand; // what you set wins, always

  std::error_code ec;
  const std::filesystem::path managed = ManagedPythonPath();
  if (std::filesystem::exists(managed, ec))
    return managed.string();

  return {}; // nothing set up: the transcriber tries whatever is on PATH
}

void MainWindow::DrawTranscribePopup()
{
  if (!mTranscribeDialogOpen)
    return;

  if (!ImGui::IsPopupOpen("Transcribe chords"))
    ImGui::OpenPopup("Transcribe chords");

  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Transcribe chords", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    mTranscribeDialogOpen = false;
    return;
  }

  if (mTranscriber.IsRunning())
  {
    theme::Spinner(ImGui::GetFrameHeight() * 1.4f);
    ImGui::SameLine();
    theme::PushHeading(1.1f);
    ImGui::TextUnformatted("Transcribing");
    theme::PopFont();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    const std::string status = mTranscriber.GetStatus();
    ImGui::TextWrapped("%s", status.empty() ? "Starting..." : status.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("The very first run downloads the model, which takes longer than the rest of "
                       "it ever will. You can close this and carry on playing.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(140, 0)))
    {
      mTranscribeDialogOpen = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Several notes at once, using Spotify's basic-pitch. Install it with "
                     "\"pip install basic-pitch\". Unlike finding a single line, this is a trained "
                     "model, so it hears a chord as a chord.");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::SetNextItemWidth(260);
  if (ImGui::SliderFloat("Onset", &mConfig.transcribeOnset, 0.05f, 0.95f, "%.2f"))
    mConfig.Save();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("How sure it has to be that a note started. Lower catches quiet notes and "
                      "also catches things nobody played");

  ImGui::SetNextItemWidth(260);
  if (ImGui::SliderFloat("Sustain", &mConfig.transcribeFrame, 0.05f, 0.95f, "%.2f"))
    mConfig.Save();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The same question about a note continuing. Lower holds notes longer");

  ImGui::SetNextItemWidth(260);
  if (ImGui::SliderFloat("Shortest note", &mConfig.transcribeMinNoteMs, 20.0f, 400.0f, "%.0f ms"))
    mConfig.Save();

  // Telling it there is nothing below your lowest string removes a whole class of wrong answers,
  // and it is the one setting here worth reaching for on purpose.
  ImGui::Spacing();
  ImGui::SetNextItemWidth(120);
  if (ImGui::DragFloat("##minhz", &mConfig.transcribeMinHz, 1.0f, 0.0f, 2000.0f,
                       mConfig.transcribeMinHz <= 0.0f ? "no floor" : "%.0f Hz"))
    mConfig.Save();
  ImGui::SameLine();
  ImGui::TextUnformatted("to");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120);
  if (ImGui::DragFloat("##maxhz", &mConfig.transcribeMaxHz, 5.0f, 0.0f, 8000.0f,
                       mConfig.transcribeMaxHz <= 0.0f ? "no ceiling" : "%.0f Hz"))
    mConfig.Save();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextUnformatted("range to look in");
  ImGui::PopStyleColor();

  ImGui::SameLine(0, 12);
  if (ImGui::SmallButton("Guitar"))
  {
    mConfig.transcribeMinHz = 80.0f; // low E, with a little room under it
    mConfig.transcribeMaxHz = 1400.0f;
    mConfig.Save();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Bass"))
  {
    mConfig.transcribeMinHz = 30.0f;
    mConfig.transcribeMaxHz = 500.0f;
    mConfig.Save();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Any"))
  {
    mConfig.transcribeMinHz = 0.0f;
    mConfig.transcribeMaxHz = 0.0f;
    mConfig.Save();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // The track is looked up now rather than held, so one closed while the dialog was open cannot
  // be transcribed out from under the player.
  std::shared_ptr<AudioFile> file;
  for (const auto& track : mEngine.GetPlayer().GetTracks())
    if (track.id == mTranscribeTrackId)
      file = track.file;

  ImGui::BeginDisabled(!file);
  if (ImGui::Button("Transcribe", ImVec2(150, 0)) && file)
  {
    PolyphonicTranscriber::Options options;
    options.command = TranscribePython();
    options.onsetThreshold = mConfig.transcribeOnset;
    options.frameThreshold = mConfig.transcribeFrame;
    options.minimumNoteMs = mConfig.transcribeMinNoteMs;
    options.minimumHz = mConfig.transcribeMinHz;
    options.maximumHz = mConfig.transcribeMaxHz;

    mPlayerError.clear();
    mTranscriber.Start(file->path, AppConfig::GetConfigFolder() / "transcribe", options,
                       mTranscribeTrackId);
  }
  ImGui::EndDisabled();
  ImGui::SetItemDefaultFocus();

  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(140, 0)))
  {
    mTranscribeDialogOpen = false;
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void MainWindow::DrawAboutPopup()
{
  if (!mAboutOpen)
    return;

  if (!ImGui::IsPopupOpen("About"))
    ImGui::OpenPopup("About");

  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  // Outlined, no title bar: the mark and the name inside say what this is, and a strip above them
  // saying it again is a second, worse heading.
  ImGui::PushStyleColor(ImGuiCol_Border, theme::Line());
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  const bool open = ImGui::BeginPopupModal("About", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  if (!open)
  {
    mAboutOpen = false; // dismissed with Escape
    return;
  }

  // --- the horizontal lockup, at the size it was drawn to be seen at ---
  //
  // The mark and the name as one object, in the proportions they were drawn in, rather than the
  // mark placed next to a heading and the spacing guessed at.
  {
    constexpr float kSize = 76.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width =
      theme::LogoLockup(ImGui::GetWindowDrawList(), pos.x, pos.y + kSize * 0.5f, kSize,
                        ImGui::GetColorU32(theme::Accent()), ImGui::GetColorU32(theme::Wood()));
    ImGui::Dummy(ImVec2(width, kSize));
  }

  ImGui::Spacing();
  ImGui::Spacing();

  // --- where the name comes from ---
  //
  // Set as a quotation, with a rule down its edge: it is what the word means, not a claim the app
  // is making about itself, and the two should not read the same.
  {
    const ImVec2 from = ImGui::GetCursorScreenPos();

    ImGui::Indent(16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped(
      "\"Woodshedding\", or shedding, is a term commonly used to describe the act of practicing "
      "some endeavor, usually in private, to improve one's proficiency in performing it. It is "
      "typically used by musicians to mean rehearsing a difficult passage repeatedly, until it can "
      "be performed flawlessly. The term is used metaphorically where \"the woodshed\" means any "
      "private place to practice without being heard by anyone else. This is based on the "
      "assumption that an actual woodshed would likely be in a remote location, away from the main "
      "house. The term is also used in other contexts.");
    ImGui::PopStyleColor();
    ImGui::Unindent(16.0f);

    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(from.x, from.y + 1.0f),
                                              ImVec2(from.x + 2.0f, ImGui::GetCursorScreenPos().y - 3.0f),
                                              ImGui::GetColorU32(theme::Accent()), 1.0f);
  }

  ImGui::Spacing();
  theme::Divider();
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Plays Neural Amp Modeler captures and cab impulse responses live through an "
                     "ASIO interface, alongside a player that separates a song into its stems, finds "
                     "its tempo, its chords and its notes, and a tuner and metronome that share the "
                     "same input.");
  ImGui::PopStyleColor();

  ImGui::Spacing();
  theme::SectionLabel("BUILT ON");

  // Named because they are what makes the app possible, and because anyone who ships this needs to
  // know what is inside it. Licences included for the same reason.
  struct Credit
  {
    const char* name;
    const char* what;
  };
  static const Credit kCredits[] = {
    {"Neural Amp Modeler", "the capture engine, by Steven Atkinson - MIT"},
    {"Dear ImGui", "the interface - MIT"},
    {"GLFW", "the window - zlib"},
    {"RtAudio", "the audio devices - MIT"},
    {"ASIO SDK", "low-latency audio on Windows, by Steinberg"},
    {"Demucs", "separating a song into stems, by Meta - MIT"},
    {"basic-pitch", "transcribing notes, by Spotify - Apache 2.0"},
    {"Beat This!", "finding the beat, by Foscarin et al. - MIT"},
    {"TONE3000", "where the captures come from"},
  };

  if (ImGui::BeginTable("##credits", 2, ImGuiTableFlags_SizingFixedFit))
  {
    for (const auto& credit : kCredits)
    {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(credit.name);
      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::TextUnformatted(credit.what);
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::Spacing();
  if (ImGui::Button("Close", ImVec2(140, 0)))
  {
    mAboutOpen = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SetItemDefaultFocus();

  ImGui::EndPopup();
}

void MainWindow::DrawSeparateStemsPopup()
{
  if (!mSeparateDialogOpen)
    return;

  if (!ImGui::IsPopupOpen("Separate stems"))
    ImGui::OpenPopup("Separate stems");

  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
  // No title bar. The heading inside says what this is, and a black strip above it saying the same
  // thing again is a second, uglier heading.
  if (!ImGui::BeginPopupModal("Separate stems", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
  {
    mSeparateDialogOpen = false; // dismissed with Escape
    return;
  }

  const bool running = mSeparator.IsRunning();

  if (running)
  {
    const float progress = mSeparator.GetProgress();
    const int pass = mSeparator.GetPass();
    const int passes = mSeparator.GetPassCount();
    const bool measured = progress >= 0.0f;

    theme::PushHeading(1.25f);
    ImGui::TextUnformatted("Separating");
    theme::PopFont();

    // Only said when it is true. Once there is a percentage the status line is whatever the tool
    // last happened to print, which is usually several steps out of date - "Reading the track"
    // next to 87% is worse than nothing beside the heading.
    if (measured && passes > 1)
    {
      ImGui::SameLine(0, 12);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint());
      ImGui::Text("pass %d of %d", std::clamp(pass, 1, passes), passes);
      ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // --- the bar, drawn rather than described ---
    //
    // The tool's own bar is Unicode block characters, which this font does not have, so what used
    // to be shown here was a row of empty boxes and a float with fifteen decimal places. The
    // numbers are parsed out of that line now and the drawing is the app's.
    const ImVec2 barFrom = ImGui::GetCursorScreenPos();
    const float barWidth = ImGui::GetContentRegionAvail().x;
    constexpr float kBarHeight = 6.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(barFrom, ImVec2(barFrom.x + barWidth, barFrom.y + kBarHeight),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.09f)), kBarHeight * 0.5f);

    if (measured)
    {
      // Each pass covers its share of the whole, so the bar fills once across the run rather than
      // resetting four times and looking like it is going backwards.
      const float share = 1.0f / static_cast<float>(std::max(1, passes));
      const float overall = std::clamp((static_cast<float>(std::max(0, pass - 1)) + progress) * share, 0.0f, 1.0f);
      draw->AddRectFilled(barFrom, ImVec2(barFrom.x + barWidth * overall, barFrom.y + kBarHeight),
                          ImGui::GetColorU32(theme::Accent()), kBarHeight * 0.5f);
    }
    else
    {
      // Nothing to measure yet: a lozenge running back and forth, which says "working" without
      // claiming to know how far along it is.
      const float t = static_cast<float>(ImGui::GetTime()) * 0.9f;
      const float sweep = 0.5f - 0.5f * std::cos(t);
      const float lozenge = barWidth * 0.28f;
      const float at = (barWidth - lozenge) * sweep;
      draw->AddRectFilled(ImVec2(barFrom.x + at, barFrom.y),
                          ImVec2(barFrom.x + at + lozenge, barFrom.y + kBarHeight),
                          ImGui::GetColorU32(theme::Accent()), kBarHeight * 0.5f);
    }
    ImGui::Dummy(ImVec2(barWidth, kBarHeight));

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    if (measured)
    {
      const float done = mSeparator.GetDoneSeconds();
      const float total = mSeparator.GetTotalSeconds();
      if (total > 0.0f)
        ImGui::Text("%.0f%%   %.0f of %.0f seconds of audio", progress * 100.0f, done, total);
      else
        ImGui::Text("%.0f%%", progress * 100.0f);
    }
    else
    {
      ImGui::TextUnformatted(mSeparator.GetStatus().c_str());
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    theme::Hint("Minutes on a processor, seconds on a graphics card. You can close this and carry "
                "on playing - the stems load when it finishes.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::Button("Close", ImVec2(140, 0)))
    {
      mSeparateDialogOpen = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return;
  }

  // --- the options, set here rather than in Settings: they belong to this run ---
  theme::PushHeading(1.25f);
  ImGui::TextUnformatted("Separate stems");
  theme::PopFont();
  ImGui::Dummy(ImVec2(0.0f, 4.0f));

  theme::Hint("Splits the first track into parts you can mute, solo and mix. The parts replace the "
              "mix in the player.");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));

  struct ModelChoice
  {
    const char* name;
    const char* description;
  };
  static const ModelChoice kModels[] = {
    {"htdemucs", "Four stems. The default, and the quickest"},
    {"htdemucs_ft", "Four stems, cleaner. Roughly four times slower"},
    {"htdemucs_6s", "Six stems - adds guitar and piano"},
    {"mdx_extra", "Four stems, a different model. Sometimes better on bass"}};

  ImGui::SetNextItemWidth(260);
  if (ImGui::BeginCombo("Model", mConfig.demucsModel.c_str()))
  {
    for (const auto& choice : kModels)
    {
      if (ImGui::Selectable(choice.name, mConfig.demucsModel == choice.name))
      {
        mConfig.demucsModel = choice.name;
        mConfig.Save();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", choice.description);
    }
    ImGui::EndCombo();
  }

  // Pulling one part out is a fraction of the work of splitting all of them, and it is usually
  // what you wanted - the bass out so you can play it yourself.
  static const char* kStemValues[] = {"", "bass", "drums", "vocals", "other"};
  static const char* kStemLabels[] = {"All stems", "Bass + rest", "Drums + rest", "Vocals + rest", "Other + rest"};

  int stemIndex = 0;
  for (int i = 0; i < IM_ARRAYSIZE(kStemValues); i++)
    if (mConfig.demucsTwoStems == kStemValues[i])
      stemIndex = i;

  ImGui::SetNextItemWidth(260);
  if (ImGui::Combo("Split", &stemIndex, kStemLabels, IM_ARRAYSIZE(kStemLabels)))
  {
    mConfig.demucsTwoStems = kStemValues[stemIndex];
    mConfig.Save();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Pulling one part out is much quicker than separating everything");

  static const char* kDeviceValues[] = {"", "cpu", "cuda"};
  static const char* kDeviceLabels[] = {"Automatic", "CPU", "GPU (CUDA)"};
  int deviceIndex = 0;
  for (int i = 0; i < IM_ARRAYSIZE(kDeviceValues); i++)
    if (mConfig.demucsDevice == kDeviceValues[i])
      deviceIndex = i;

  ImGui::SetNextItemWidth(260);
  if (ImGui::Combo("Device", &deviceIndex, kDeviceLabels, IM_ARRAYSIZE(kDeviceLabels)))
  {
    mConfig.demucsDevice = kDeviceValues[deviceIndex];
    mConfig.Save();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A GPU turns minutes into seconds, where there is one Demucs can use");

  ImGui::SetNextItemWidth(260);
  if (ImGui::SliderInt("Shifts", &mConfig.demucsShifts, 0, 5, mConfig.demucsShifts == 0 ? "off" : "%d"))
    mConfig.Save();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Averages several passes at shifted offsets. Cleaner, and this many times slower");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  const auto tracks = mEngine.GetPlayer().GetTracks();
  const bool ready = !tracks.empty() && tracks.front().file;

  ImGui::BeginDisabled(!ready);
  if (ImGui::Button("Start", ImVec2(140, 0)) && ready)
  {
    const std::filesystem::path root = mConfig.stemsFolder.empty()
                                         ? AppConfig::GetConfigFolder() / "stems"
                                         : std::filesystem::path(mConfig.stemsFolder);
    StemSeparator::Options options;
    options.command = mConfig.demucsCommand;
    options.model = mConfig.demucsModel;
    options.twoStems = mConfig.demucsTwoStems;
    options.device = mConfig.demucsDevice;
    options.shifts = mConfig.demucsShifts;

    mPlayerError.clear();
    mSeparator.Start(tracks.front().file->path, root, options);
  }
  ImGui::EndDisabled();
  ImGui::SetItemDefaultFocus();

  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(140, 0)))
  {
    mSeparateDialogOpen = false;
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void MainWindow::DrawPlayerView()
{
  Player& player = mEngine.GetPlayer();

  ImGui::BeginChild("##player", ImVec2(0, 0), true);

  DrawProjectBar();
  ImGui::Separator();

  // The toolbar is gone: opening, saving and separating live behind the project name, and the
  // grid's numbers live in the corner above the track names. What is left of both is the result
  // each of them delivers, collected here.
  {
    std::vector<std::filesystem::path> stems;
    if (mSeparator.Consume(stems))
    {
      // The mix is replaced by its parts rather than joined by them: keeping both would double
      // everything you hear.
      if (!stems.empty())
      {
        player.Clear();
        mSelectedLoop = -1;
        for (const auto& stem : stems)
          AddPlayerTrackFromPath(stem);
      }
      if (!mSeparator.GetError().empty())
        mPlayerError = mSeparator.GetError();

      mSeparateDialogOpen = false; // the dialog has nothing left to report
    }
  }

  if (!mPlayerError.empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    ImGui::TextWrapped("%s", mPlayerError.c_str());
    ImGui::PopStyleColor();
  }

  // --- tempo and grid ---
  if (mTempoAnalyser.Consume(mTempo))
    mGridMultiple = 1.0f; // a fresh answer is by definition what was detected

  // The deeper pass answers the same question, so it simply replaces the answer - and the grid,
  // the click and anything worked out from the beats follow it without knowing which found them.
  {
    TempoEstimate deep;
    if (mDeepBeats.Consume(deep) && deep.valid)
    {
      // The bar length it actually heard, so the click counts the same bars the grid draws. Taken
      // as the commonest gap between downbeats rather than the average: a song in four with one
      // bar of two in it is still a song in four.
      if (deep.downbeats.size() > 8)
      {
        std::array<int, 13> votes{};
        int since = 0;
        for (size_t i = 0; i < deep.downbeats.size(); i++)
        {
          if (deep.downbeats[i] == 0)
          {
            since++;
            continue;
          }
          if (since >= 1 && since <= 12)
            votes[static_cast<size_t>(since)]++;
          since = 1;
        }

        int best = 0;
        for (int length = 2; length <= 12; length++)
          if (votes[static_cast<size_t>(length)] > votes[static_cast<size_t>(best)])
            best = length;
        if (best >= 2)
          mGridBeatsPerBar = best;
      }

      mTempo = std::move(deep);
      mGridMultiple = 1.0f;
      mProjectDirty = true;
      RebuildSynthScore(); // chords were placed on the old beats
    }
  }

  // The click's tempo is the song's tempo, always - there is no button that has to be pressed for
  // the two to agree, and therefore no state in which they quietly do not.
  if (mTempo.valid)
    SendTempoToMetronome();

  {
  }

  {
    // A finished note analysis takes over that track's lane. Collected here rather than in the
    // track list, so it lands whether or not that lane happened to be drawn this frame.
    int analysedTrack = 0;
    NoteTrack found;
    if (mNoteAnalyser.Consume(analysedTrack, found))
    {
      // Judged on what was found rather than on what survived the cleanup: a run that heard plenty
      // and cleaned it all away is a setting to move, not an analysis to throw out.
      if (found.rawNotes.empty())
      {
        mPlayerError = "No single notes found in that track. It hears one note at a time - try a "
                       "bass or a vocal stem, or find the chords instead.";
      }
      else
      {
        // Whatever chords were already found stay: they answer a different question about the
        // same track, and finding one should not throw away the other.
        NoteTrack& target = mTrackNotes[analysedTrack];
        const std::vector<DetectedChord> chords = target.chords;
        const NoteTrack previous = target;
        target = std::move(found);
        target.chords = chords;
        // Playback settings belong to the track, not to the analysis that happened to be running.
        target.playNotes = previous.playNotes;
        target.playChords = previous.playChords;
        target.instrument = previous.instrument;
        target.playGainDb = previous.playGainDb;
        target.playOctave = previous.playOctave;
        RebuildSynthScore();
      }
    }

    int chordTrack = 0;
    std::vector<DetectedChord> chords;
    if (mChordAnalyser.Consume(chordTrack, chords))
    {
      if (chords.empty())
        mPlayerError = "No chords found in that track - it may be too quiet, or a drum stem.";
      else
        mTrackNotes[chordTrack].chords = std::move(chords);
      mTrackNotes[chordTrack].visible = true;
      RebuildSynthScore();
    }

    int transcribedTrack = 0;
    NoteTrack transcribed;
    if (mTranscriber.Consume(transcribedTrack, transcribed))
    {
      if (!transcribed.rawNotes.empty())
      {
        const NoteTrack previous = mTrackNotes[transcribedTrack];
        mTrackNotes[transcribedTrack] = std::move(transcribed);
        mTrackNotes[transcribedTrack].chords = previous.chords;
        mTrackNotes[transcribedTrack].playNotes = previous.playNotes;
        mTrackNotes[transcribedTrack].playChords = previous.playChords;
        mTrackNotes[transcribedTrack].instrument = previous.instrument;
        mTrackNotes[transcribedTrack].playGainDb = previous.playGainDb;
        mTrackNotes[transcribedTrack].playOctave = previous.playOctave;
        RebuildSynthScore();
      }
      if (!mTranscriber.GetError().empty())
        mPlayerError = mTranscriber.GetError();

      mTranscribeDialogOpen = false; // the dialog has nothing left to report
    }
  }

  // --- tracks on the left, timeline filling the rest ---
  // Both panes are the same height and split it into the same number of lanes, so every track's
  // controls sit level with its own waveform.
  auto tracks = player.GetTracks();

  // The click is a channel too, always at the foot of the stack. It is not one of the player's
  // tracks - it is mixed in after them - so it is counted here rather than being in that list,
  // which is also what keeps it from being dragged out of place.
  const bool hasClickLane = !tracks.empty();
  const size_t laneCount = std::max<size_t>(1, tracks.size() + (hasClickLane ? 1u : 0u));
  const size_t trackCount = laneCount;
  const float trackWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.20f, 190.0f, 300.0f);

  auto loops = player.GetLoops();
  // What is left after the horizontal scrollbar under the panes. The transport is no longer this
  // view's problem - it is reserved for one level up, under every view.
  const float bodyHeight =
    ImGui::GetContentRegionAvail().y - kScrollBarThickness - ImGui::GetStyle().ItemSpacing.y * 2.0f;

  // Worked out once, from the inner height both children will actually have. The ruler comes off
  // the top of both: the timeline draws it, and the controls leave a gap the same size so every
  // lane still lines up with its own waveform.
  const float paneInnerHeight = bodyHeight - ImGui::GetStyle().WindowPadding.y * 2.0f;
  const float lanesViewport = std::max(1.0f, paneInnerHeight - kRulerHeight);
  const float laneHeight = LaneHeight(trackCount, lanesViewport);
  const float lanesContent = laneHeight * static_cast<float>(trackCount);

  // How far down the lanes can go, and the wheel that gets you there. Held to it every frame so
  // closing a track cannot leave the view scrolled past the end of what is left.
  const float maxLaneScroll = std::max(0.0f, lanesContent - lanesViewport);
  mLaneScroll = std::clamp(mLaneScroll, 0.0f, maxLaneScroll);

  // A drag ends when the button comes up, wherever the pointer happens to be by then - releasing
  // outside the lane you started in is the normal way to finish one.
  if (mDraggingTrackId != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    mDraggingTrackId = 0;

  // Where the two panes begin and end on screen, for the selection outline that spans them both.
  const ImVec2 panesOrigin = ImGui::GetCursorScreenPos();

  ImGui::BeginChild("##tracks", ImVec2(trackWidth, bodyHeight), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  {
    // Nothing here when there are no tracks: the timeline beside it already offers the two ways in,
    // and a paragraph explaining the empty state next to buttons that resolve it is noise.

    // Screen coordinates, from the same starting point the timeline measures from. Both panes sit
    // on one line with the same padding, so their content tops are the same pixel - deriving each
    // one's lane positions separately worked only for as long as nothing in either drifted.
    const ImVec2 tracksOrigin = ImGui::GetCursorScreenPos();

    // The strip level with the ruler is left empty. The grid used to be read here, which put it as
    // far from the channel that plays it as the pane allows; it is on that channel now.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, kRulerHeight));
    const float top = ImGui::GetCursorPosY() + kRulerHeight;
    for (size_t t = 0; t < tracks.size(); t++)
    {
      Track track = tracks[t];
      ImGui::PushID(track.id);

      // Pinned to the top of its lane rather than flowing after the one above, which is what
      // keeps it level with the waveform however tall the lanes happen to be - and scrolled by
      // the same number the waveforms use, so the two cannot come apart.
      // The lane's top edge, in screen space, exactly as the timeline works it out.
      const float stripTop = tracksOrigin.y + kRulerHeight + laneHeight * static_cast<float>(t) - mLaneScroll;
      const float laneTop = top + laneHeight * static_cast<float>(t) - mLaneScroll;
      // Inset from the pane's left edge, so the M button clears the lane's outline and the black
      // margin beside it rather than sitting half under them.
      ImGui::SetCursorScreenPos(ImVec2(tracksOrigin.x + 8.0f, stripTop + 5.0f));

      bool changed = false;

      // Whether the pointer is anywhere in this strip. The occasional controls appear on it and
      // are gone otherwise: EQ and the note finder are things you reach for a few times a session,
      // and eight tracks of them sitting out is sixteen buttons competing with the two that
      // actually get used.
      const ImVec2 pointer = ImGui::GetIO().MousePos;
      const bool stripHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && pointer.y >= stripTop
                                && pointer.y < stripTop + laneHeight;

      // --- M and S first, then the name: the mixer's order, and the order they are reached in ---
      const float toggleSize = ImGui::GetFrameHeight();
      changed |= theme::LetterToggle("##mute", "M", &track.muted, toggleSize, theme::Accent());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mute (M)");
      ImGui::SameLine(0, 4);
      changed |= theme::LetterToggle("##solo", "S", &track.soloed, toggleSize, theme::Control());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Solo (S)");
      ImGui::SameLine(0, 10);

      // Room at the right for the three that come and go.
      const float actionsWidth = toggleSize * 3.0f + 10.0f;

      // --- the name: a label until you ask to change it ---
      if (mRenamingTrackId == track.id)
      {
        ImGui::SetNextItemWidth(-actionsWidth);
        if (mTrackRenameFocus)
        {
          ImGui::SetKeyboardFocusHere();
          mTrackRenameFocus = false;
        }
        // No ring round the field while it is focused. The caret and the selected text already say
        // it is being typed into, and the accent means "this is the channel you are on" everywhere
        // else in this view - two different things in one colour, one inside the other.
        ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, theme::Line());
        const bool entered = ImGui::InputText("##rename", mTrackNameInput, sizeof(mTrackNameInput),
                                              ImGuiInputTextFlags_EnterReturnsTrue
                                                | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleColor(2);
        if (entered)
        {
          track.name = mTrackNameInput;
          changed = true;
          mRenamingTrackId = 0;
        }
        // Clicking away is a finish, not a cancel - the same as it is everywhere else.
        if (ImGui::IsItemDeactivated())
        {
          if (mRenamingTrackId == track.id)
          {
            track.name = mTrackNameInput;
            changed = true;
          }
          mRenamingTrackId = 0;
        }
      }
      else
      {
        const std::string label = track.name.empty() ? (track.file ? track.file->name : std::string("?")) : track.name;
        // Never drawn as selected. A filled block behind the name says "this word is chosen" when
        // what is chosen is the whole channel - the outline further down says that instead.
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(std::max(30.0f, ImGui::GetContentRegionAvail().x - actionsWidth), 0)))
        {
          mSelectedTrackId = track.id;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          {
            mRenamingTrackId = track.id;
            mTrackRenameFocus = true;
            std::snprintf(mTrackNameInput, sizeof(mTrackNameInput), "%s", label.c_str());
          }
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s\nDouble-click to rename, drag to reorder",
                            track.file ? track.file->name.c_str() : "");

        // --- dragging a track into another place in the order ---
        //
        // Reordered as you drag rather than on release: the lanes move under the pointer, so what
        // you are looking at while you drag is the arrangement you will get.
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
          mDraggingTrackId = track.id;

          const float pointer = ImGui::GetMousePos().y - ImGui::GetWindowPos().y + ImGui::GetScrollY();
          const float overLane = (pointer - top + mLaneScroll) / laneHeight;
          const auto wanted =
            static_cast<size_t>(std::clamp(overLane, 0.0f, static_cast<float>(tracks.size() - 1)));

          if (wanted != t)
          {
            player.MoveTrack(track.id, wanted);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
          }
        }
        else if (mDraggingTrackId == track.id && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
          mDraggingTrackId = 0;
        }
      }

      // The lane being carried, so it is obvious which one is moving.
      if (mDraggingTrackId == track.id)
      {
        ImDrawList* lanes = ImGui::GetWindowDrawList();
        const ImVec2 from(ImGui::GetWindowPos().x, stripTop);
        lanes->AddRectFilled(from, ImVec2(from.x + ImGui::GetWindowWidth(), from.y + laneHeight),
                             ImGui::GetColorU32(ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.10f)));
      }

      // --- the selected channel, outlined ---
      //
      // Drawn in this pane's own list rather than on the foreground, so a window opened over the
      // top of it - a track's EQ, say - is in front of the outline rather than behind it.
      //
      // The right-hand edge is left open: the lane half of the outline is drawn by the timeline,
      // and the two together read as one rectangle round the whole channel.
      if (mSelectedTrackId == track.id)
      {
        ImDrawList* lanes = ImGui::GetWindowDrawList();
        const float left = ImGui::GetWindowPos().x + 1.0f;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
        const ImU32 edge = ImGui::GetColorU32(theme::Accent());
        lanes->AddLine(ImVec2(left, stripTop + 1.0f), ImVec2(right, stripTop + 1.0f), edge, 1.5f);
        lanes->AddLine(ImVec2(left, stripTop + laneHeight - 1.0f), ImVec2(right, stripTop + laneHeight - 1.0f), edge,
                       1.5f);
        lanes->AddLine(ImVec2(left, stripTop + 1.0f), ImVec2(left, stripTop + laneHeight - 1.0f), edge, 1.5f);
      }

      // --- the three that come and go, right-aligned on the name's row ---
      const auto notesFound = mTrackNotes.find(track.id);
      const bool hasNotes = notesFound != mTrackNotes.end() && !notesFound->second.notes.empty();
      // Notes cleaned away still count as something found: otherwise a cleanup turned all the way
      // up would take the menu it was set from away with the last note.
      const bool hasAnything = hasNotes
                               || (notesFound != mTrackNotes.end()
                                   && (!notesFound->second.chords.empty() || !notesFound->second.rawNotes.empty()));
      const bool notesShown = hasAnything && notesFound->second.visible;

      const bool busy = mNoteAnalyser.RunningTrackId() == track.id || mTranscriber.RunningTrackId() == track.id
                        || mChordAnalyser.RunningTrackId() == track.id;

      // Shown while the pointer is in the strip, and otherwise only when they are switched on -
      // so a lane at rest is its name and its level, and the state is still visible.
      const bool showActions = stripHovered || busy || track.eqEnabled || notesShown;

      ImGui::SameLine();
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - actionsWidth + 4.0f);

      if (busy)
      {
        theme::Spinner(toggleSize);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", mTranscriber.RunningTrackId() == track.id
                                    ? "Transcribing. The first run downloads the model"
                                    : "Listening through the track for its notes");
      }
      else if (showActions)
      {
        if (theme::IconButton("##notes", theme::Icon::Note, toggleSize, theme::IconStyle::Bare, notesShown))
        {
          if (hasAnything)
            notesFound->second.visible = !notesFound->second.visible; // already found: this shows them
          else if (track.file)
            mChordAnalyser.Start(track.file, track.id, BeatTimes()); // chords are what most people want
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(hasAnything ? "Show or hide what was found in this track. Right-click for more"
                                        : "Work out the chords in this track. Right-click for single notes "
                                          "or for a full transcription");

        // Everything else this button can do. A menu rather than more buttons, because the lane is
        // as narrow as it is and none of these is a thing you do twice a minute.
        if (ImGui::BeginPopupContextItem("##notesmenu"))
        {
          if (ImGui::MenuItem("Find chords", nullptr, false, track.file != nullptr))
          {
            mTrackNotes[track.id].chords.clear();
            mChordAnalyser.Start(track.file, track.id, BeatTimes());
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Chord names across the track. Works on a full mix as well as a stem, "
                              "takes about a second, and needs nothing installed");

          if (ImGui::MenuItem("Find single notes", nullptr, false, track.file != nullptr))
          {
            mTrackNotes[track.id].notes.clear();
            mTrackNotes[track.id].rawNotes.clear();
            mNoteAnalyser.Start(track.file, track.id);
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("One note at a time - a bass line or a vocal");

          if (ImGui::MenuItem("Transcribe every note...", nullptr, false, track.file != nullptr))
          {
            mTranscribeTrackId = track.id;
            mTranscribeDialogOpen = true;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every note of every voice, using basic-pitch. Needs Python, and takes a while");

          // --- how much of what was found is worth showing ---
          //
          // Every detector reports more than was played: the harmonics of loud notes, the reverb
          // after a phrase, a scatter of fragments where it was unsure. How much of that to throw
          // away depends on the recording, not on the detector, so it is a control rather than a
          // constant - and it works over what was already found, so moving it is instant where
          // transcribing again is minutes.
          if (notesFound != mTrackNotes.end() && !notesFound->second.rawNotes.empty())
          {
            NoteTrack& found = notesFound->second;

            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
            ImGui::TextUnformatted("CLEAN UP");
            ImGui::PopStyleColor();

            bool recleaned = false;

            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("Strength", &found.cleanupStrength, 0.0f, 1.0f, "%.2f"))
              recleaned = true;
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Throws away harmonics of louder notes, and anything too quiet or "
                                "too short to have been played. Fully left only joins back up the "
                                "notes that were reported twice");

            // Only worth asking of a transcriber. A single-line detector cannot produce a second
            // note at all, so a limit on how many sound at once would be a control over nothing.
            if (found.polyphonic)
            {
              ImGui::SetNextItemWidth(160);
              if (ImGui::SliderInt("Most at once", &found.maxVoices, 0, 8,
                                   found.maxVoices == 0 ? "no limit" : "%d"))
                recleaned = true;
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Six is a guitar, four is most keyboard playing, one is a single "
                                  "line. Past this, the quietest notes at that moment go");
            }

            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
            ImGui::Text("%d of %d notes", static_cast<int>(found.notes.size()),
                        static_cast<int>(found.rawNotes.size()));
            ImGui::PopStyleColor();

            if (recleaned)
            {
              ApplyNoteCleanup(found);
              RebuildSynthScore();
            }
          }

          if (hasAnything)
          {
            ImGui::Separator();
            if (DrawPlaybackControls(notesFound->second))
              RebuildSynthScore();
          }

          // Throwing away one kind and keeping the other: the two are found separately and take
          // different amounts of work to get back, so "clear" that took both was too blunt.
          const bool anyNotes = notesFound != mTrackNotes.end() && !notesFound->second.rawNotes.empty();
          const bool anyChords = notesFound != mTrackNotes.end() && !notesFound->second.chords.empty();

          if (anyNotes || anyChords)
          {
            ImGui::Separator();
            if (ImGui::MenuItem("Remove the notes", nullptr, false, anyNotes))
            {
              notesFound->second.notes.clear();
              notesFound->second.rawNotes.clear();
              if (notesFound->second.chords.empty())
                mTrackNotes.erase(track.id);
              RebuildSynthScore();
            }
            if (ImGui::MenuItem("Remove the chords", nullptr, false, anyChords))
            {
              notesFound->second.chords.clear();
              if (notesFound->second.rawNotes.empty())
                mTrackNotes.erase(track.id);
              RebuildSynthScore();
            }
            if (ImGui::MenuItem("Remove both", nullptr, false, anyNotes && anyChords))
            {
              mTrackNotes.erase(track.id);
              RebuildSynthScore();
            }
          }

          ImGui::EndPopup();
        }
      }

      // --- the track's own five bands ---
      if (showActions && !busy)
      {
        ImGui::SameLine(0, 3);
        if (theme::IconButton("##eq", theme::Icon::Sliders, toggleSize, theme::IconStyle::Bare, track.eqEnabled))
        {
          mSelectedTrackId = track.id;
          mEqTrackId = track.id;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("This track's EQ. Right-click to switch it off without opening it");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
          track.eqEnabled = !track.eqEnabled;
          changed = true;
        }
      }

      // Removing a track is not something to have under the pointer while reaching for anything
      // else, so it appears with the rest and sits furthest out.
      bool removed = false;
      if (stripHovered && !busy)
      {
        ImGui::SameLine(0, 3);
        if (theme::CloseButton("remove", toggleSize))
        {
          player.RemoveTrack(track.id);
          mTrackNotes.erase(track.id);
          RebuildSynthScore();
          if (mSelectedTrackId == track.id)
            mSelectedTrackId = 0;
          if (mEqTrackId == track.id)
            mEqTrackId = 0;
          removed = true;
        }
      }

      // --- the level, on a row of its own across the full width ---
      if (!removed)
      {
        ImGui::SetCursorScreenPos(ImVec2(tracksOrigin.x, stripTop + toggleSize + 11.0f));
        changed |= theme::SlimSlider("##gain", &track.gainDb, -40.0f, 12.0f, 0.0f,
                                     std::max(40.0f, ImGui::GetContentRegionAvail().x - 4.0f), "%+.1f dB");
        if (ImGui::IsItemActive())
          mSelectedTrackId = track.id;

        if (changed)
        {
          player.SetTrack(track);
          mProjectDirty = true;
        }

        // --- the one thing to do with a song that is still one track ---
        //
        // An imported song arrives as a single lane and the next step is almost always to pull it
        // apart. Offered under that channel's own level, where everything else about the channel
        // is done. It stops being offered the moment there is more than one track, which is
        // exactly when it has been done.
        if (t == 0 && tracks.size() == 1)
        {
          ImGui::SetCursorScreenPos(ImVec2(tracksOrigin.x, stripTop + toggleSize + 38.0f));

          if (mSeparator.IsRunning())
          {
            // Reported where the offer was made. A run shown somewhere else in the window would
            // leave this channel looking as though nothing had been asked of it.
            if (!mSeparateDialogOpen)
            {
              constexpr float kSpinner = 22.0f;
              if (theme::SpinnerButton("##separating", kSpinner))
                mSeparateDialogOpen = true;
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Separating stems. Click to watch it");

              const float progress = mSeparator.GetProgress();
              if (progress >= 0.0f)
              {
                ImGui::SameLine(0, 8);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kSpinner - ImGui::GetFontSize()) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
                ImGui::Text("%.0f%%", progress * 100.0f);
                ImGui::PopStyleColor();
              }
            }
          }
          else if (ImGui::Button("Separate stems", ImVec2(std::max(40.0f, ImGui::GetContentRegionAvail().x - 4.0f), 0)))
          {
            mSeparateDialogOpen = true;
          }
          if (!mSeparator.IsRunning() && ImGui::IsItemHovered())
            ImGui::SetTooltip("Split this song into drums, bass, vocals and the rest");
        }
      }

      ImGui::PopID();
    }

    // --- the click, as the last channel in the stack ---
    //
    // Mute, solo and a level, in the same places as every other channel's, because that is what it
    // is: a part of what you are hearing. Mute is the same state the C key toggles, so the two
    // cannot disagree - and its level is the metronome's own, so setting it here and setting it in
    // the metronome view are the same act.
    if (hasClickLane)
    {
      ImGui::PushID("clicklane");

      const size_t t = tracks.size();
      const float stripTop = tracksOrigin.y + kRulerHeight + laneHeight * static_cast<float>(t) - mLaneScroll;
      ImGui::SetCursorScreenPos(ImVec2(tracksOrigin.x + 8.0f, stripTop + 5.0f));

      const float toggleSize = ImGui::GetFrameHeight();

      // The button is the mute, so it is lit when the channel is muted - the same as every M in
      // the stack. It was bound to "armed" instead, which lit it when the click was audible and
      // read as the exact opposite of what it did.
      bool muted = !mClickArmed;
      if (theme::LetterToggle("##clickmute", "M", &muted, toggleSize, theme::Accent()))
        mClickArmed = !muted;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(mClickArmed ? "The click is in the mix (C)" : "The click is muted (C)");

      ImGui::SameLine(0, 4);
      bool soloed = mMetronomeSolo;
      if (theme::LetterToggle("##clicksolo", "S", &soloed, toggleSize, theme::Control()))
        mMetronomeSolo = soloed;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The click on its own");

      ImGui::SameLine(0, 10);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, mClickArmed ? theme::Text() : theme::TextDim());
      ImGui::TextUnformatted("Click");
      ImGui::PopStyleColor();

      ImGui::SetCursorScreenPos(ImVec2(tracksOrigin.x, stripTop + toggleSize + 11.0f));
      if (theme::SlimSlider("##clicklevel", &mMetronome.levelDb, -40.0f, 6.0f, 0.0f,
                            std::max(40.0f, ImGui::GetContentRegionAvail().x - 4.0f), "%+.1f dB"))
        ApplyMetronome();
      if (ImGui::IsItemDeactivatedAfterEdit())
        mConfig.Save();

      // The grid, on the channel that plays it: a button that finds it, and once it is found the
      // same place reads it back and opens the corrections.
      ImGui::SetCursorScreenPos(ImVec2(tracksOrigin.x, stripTop + toggleSize + 38.0f));
      const float clickWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x - 4.0f);
      DrawTempoPanel(clickWidth);

      // The counting buttons are on the lane itself, over the beats they change - see the timeline.

      ImGui::PopID();
    }
  }
  ImGui::EndChild();

  // Only the tracks are silenced, and only while the click is soloed - the flag is separate from
  // every track's own settings, so letting go of it puts the mix back exactly as it was.
  player.SetSilenced(mMetronomeSolo && hasClickLane);

  ImGui::SameLine();
  // Room for the vertical scrollbar down the right-hand edge.
  const float timelineWidth = ImGui::GetContentRegionAvail().x - kScrollBarThickness - 4.0f;
  // Black behind the lanes rather than a panel. Each lane already brings its own shade, so a grey
  // plate under them only lifts the floor everything is measured against.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Base());
  const float panesRight = ImGui::GetCursorScreenPos().x + timelineWidth;
  ImGui::BeginChild("##timelinepane", ImVec2(timelineWidth, bodyHeight), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  {
    // The ruler plus as much lane as there is room for; what does not fit is scrolled to. The lane
    // height comes from outside so both panes are laying out to the same number.
    const ImVec2 paneOrigin = ImGui::GetCursorScreenPos();
    const float paneHeight = ImGui::GetContentRegionAvail().y;
    const float paneWidth = ImGui::GetContentRegionAvail().x;
    DrawTimeline(paneWidth, paneHeight, laneHeight);

    // --- the wheel ---
    //
    // Bare it scrolls, Shift turns it sideways, and Ctrl makes it zoom - Shift picking the axis
    // there too. That is the pairing every timeline uses, and the scrollbars still zoom the same
    // two things, so neither way has to be learned instead of the other.
    //
    // Both zooms hold whatever is under the pointer still. Zooming about the middle of the pane
    // means the bar you were looking at slides away and has to be chased back with a scroll.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool overLanes = ImGui::IsWindowHovered() && mouse.y > paneOrigin.y + kRulerHeight;
    const float wheel = io.MouseWheel;

    if (overLanes && wheel != 0.0f)
    {
      // A notch is 15% either way, which is small enough to land where you meant and large enough
      // to cross a song in a few turns.
      const float factor = std::pow(0.85f, wheel);

      if (io.KeyCtrl && io.KeyShift)
      {
        // Taller or shorter lanes. The pointer's own lane stays under the pointer.
        const float offset = mouse.y - (paneOrigin.y + kRulerHeight);
        const float lane = (mLaneScroll + offset) / std::max(1.0f, laneHeight);

        mLaneHeight = std::clamp(laneHeight * factor, 24.0f, 400.0f);

        const float content = mLaneHeight * static_cast<float>(std::max<size_t>(1, trackCount));
        mLaneScroll = std::clamp(lane * mLaneHeight - offset, 0.0f, std::max(0.0f, content - lanesViewport));
      }
      else if (io.KeyCtrl)
      {
        const double duration = player.DurationSeconds();
        if (duration > 0.0 && paneWidth > 0.0f)
        {
          const double fraction = static_cast<double>((mouse.x - paneOrigin.x) / paneWidth);
          const double at = mViewStart + fraction * mViewDuration;

          // Half a second across the pane is as far in as anything is worth looking at, and the
          // whole song is as far out as there is.
          mViewDuration = std::clamp(mViewDuration * static_cast<double>(factor), std::min(0.5, duration), duration);
          mViewStart = std::clamp(at - fraction * mViewDuration, 0.0, duration - mViewDuration);
          mFollowPlayhead = false;
        }
      }
      else if (io.KeyShift)
      {
        // Sideways, by a tenth of what is on screen per notch.
        const double step = mViewDuration * 0.1 * static_cast<double>(-wheel);
        mViewStart = std::clamp(mViewStart + step, 0.0, std::max(0.0, player.DurationSeconds() - mViewDuration));
        mFollowPlayhead = false;
      }
      else
      {
        mLaneScroll = std::clamp(mLaneScroll - wheel * laneHeight * 0.6f, 0.0f, maxLaneScroll);
      }
    }
  }
  ImGui::EndChild();

  // --- the two scrollbars ---
  //
  // Drawn rather than left to ImGui, because the vertical one moves two panes at once and the
  // horizontal one scrolls a view onto time rather than a wider piece of content.
  // --- the vertical bar: how tall the lanes are, and which of them you are looking at ---
  //
  // Always there, even when everything already fits, because it is the only way to make the lanes
  // taller - a control that disappears when the view happens to be full is a control you cannot
  // find when you want it.
  ImGui::SameLine();
  {
    const ImVec2 verticalAt = ImGui::GetCursorScreenPos();
    float content = std::max(lanesViewport, lanesContent);
    float sizeFraction = std::clamp(lanesViewport / content, 0.02f, 1.0f);
    float startFraction = (content > 0.0f) ? mLaneScroll / content : 0.0f;

    // Not so far that a lane is a hairline, and not so far in that one fills the window twice over.
    const float smallest = lanesViewport / (400.0f * static_cast<float>(trackCount));

    if (ZoomScrollBar("##laneScroll", ImVec2(verticalAt.x, verticalAt.y + kRulerHeight),
                      bodyHeight - kRulerHeight, true, startFraction, sizeFraction,
                      std::clamp(smallest, 0.02f, 1.0f)))
    {
      // The window on the content is fixed - it is the pane. Showing less of the content means the
      // content got taller, which is what lane height is.
      content = lanesViewport / std::max(0.02f, sizeFraction);
      mLaneHeight = std::clamp(content / static_cast<float>(trackCount), 24.0f, 400.0f);
      mLaneScroll = startFraction * content;
    }
  }

  {
    // --- the horizontal bar: how much of the song is on screen, and which part ---
    const double duration = player.DurationSeconds();
    if (duration > 0.0)
    {
      float sizeFraction = static_cast<float>((mViewDuration > 0.0 ? mViewDuration : duration) / duration);
      float startFraction = static_cast<float>(mViewStart / duration);

      const ImVec2 at = ImGui::GetCursorScreenPos();
      // Half a second is as far in as anything is worth looking at.
      const float smallest = static_cast<float>(std::min(1.0, 0.5 / duration));

      if (ZoomScrollBar("##timeScroll", ImVec2(at.x + trackWidth, at.y), timelineWidth, false, startFraction,
                        sizeFraction, std::clamp(smallest, 0.0005f, 1.0f)))
      {
        mViewDuration = static_cast<double>(sizeFraction) * duration;
        mViewStart = static_cast<double>(startFraction) * duration;
        mFollowPlayhead = false; // taking hold of the view means you want it where you put it
      }
    }
    else
    {
      // Nothing loaded: the bar still takes its row, so the transport does not move when a track
      // arrives.
      ImGui::Dummy(ImVec2(0.0f, kScrollBarThickness));
    }
  }

  // The loops used to be listed here, in a strip that grew downward as you made them - so working
  // on a song took the timeline away a row at a time and moved the transport while you used it.
  // Every editor puts a cycle region in the ruler and leaves it there, which is where these are
  // now drawn; renaming and deleting is on their right-click menu, and stepping between them is on
  // the transport.

  ImGui::EndChild();

  // Opened outside the panes, so no dialog is clipped by whichever child raised it.
  DrawSeparateStemsPopup();
  DrawTranscribePopup();
  DrawSaveProjectPopup();

  // --- starting again, when there is something to lose ---
  if (mConfirmNewOpen)
  {
    if (!ImGui::IsPopupOpen("Start again"))
      ImGui::OpenPopup("Start again");

    if (ImGui::BeginPopupModal("Start again", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
      theme::Hint(mProjectFile.empty()
                    ? "This project has never been saved. Starting again closes the tracks, the "
                      "loops and everything that has been worked out from them."
                    : "There are changes since this project was last saved. Starting again "
                      "closes them.");
      ImGui::Dummy(ImVec2(0.0f, 6.0f));

      if (ImGui::Button("Save first", ImVec2(130, 0)))
      {
        std::snprintf(mProjectNameInput, sizeof(mProjectNameInput), "%s",
                      mProjectName.empty() ? "New project" : mProjectName.c_str());
        mSaveProjectDialogOpen = true;
        mConfirmNewOpen = false;
        ImGui::CloseCurrentPopup();
      }

      ImGui::SameLine();
      if (ImGui::Button("Start again", ImVec2(130, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
      {
        StartNewProject();
        mConfirmNewOpen = false;
        ImGui::CloseCurrentPopup();
      }

      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
        mConfirmNewOpen = false;
        ImGui::CloseCurrentPopup();
      }

      ImGui::EndPopup();
    }
    else
    {
      mConfirmNewOpen = false;
    }
  }
}

void MainWindow::ApplyMetronome()
{
  MetronomeSettings settings;
  settings.running = mMetronomeRunning;
  settings.bpm = mMetronome.bpm;
  settings.beatsPerBar = mMetronome.beatsPerBar;
  settings.beatUnit = mMetronome.beatUnit;
  settings.subdivision = mMetronome.subdivision;
  settings.levelDb = mMetronome.levelDb;

  const auto voice = [](int index)
  { return index == 1 ? ClickVoice::Wood : index == 2 ? ClickVoice::Tick : ClickVoice::Beep; };

  settings.accent.voice = voice(mMetronome.accentVoice);
  settings.accent.levelDb = mMetronome.accentDb;
  settings.beat.voice = voice(mMetronome.beatVoice);
  settings.beat.levelDb = mMetronome.beatDb;
  settings.sub.voice = voice(mMetronome.subVoice);
  settings.sub.levelDb = mMetronome.subDb;
  settings.sub.enabled = mMetronome.subEnabled && mMetronome.subdivision > 1;

  mEngine.SetMetronome(settings);

  mConfig.metronome = mMetronome;
  mConfig.metronomeVisual = mMetronomeVisual;
}

void MainWindow::DrawPendulum(float width, float height, float phase, int beat, bool running)
{
  constexpr float kPi = 3.14159265358979323846f;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // No panel behind it. On black, an arm swinging in space is the whole picture; a grey rectangle
  // around it only draws a box you then have to look past.
  //
  // Hung from the top of the area rather than pivoting near its middle: given real height the arm
  // wants length, and a long arm swinging under a fixed point is the shape a pendulum has.
  const ImVec2 pivot = ImVec2(origin.x + width * 0.5f, origin.y + height * 0.055f);
  const float length = height * 0.82f;
  const float swing = kPi / 7.0f; // about 26 degrees each way, like the real thing

  // A pendulum crosses the middle on the beat and is at the end of its travel between beats, and
  // it alternates sides. Cosine gives that shape for free: the ends are where it slows down.
  const float side = (beat % 2 == 0) ? 1.0f : -1.0f;
  const float angle = running ? side * swing * std::cos(phase * kPi) : 0.0f;

  const ImVec2 bob = ImVec2(pivot.x + std::sin(angle) * length, pivot.y + std::cos(angle) * length);

  // The arc it travels, so the swing reads even when it is standing still.
  draw->PathArcTo(pivot, length, kPi * 0.5f - swing, kPi * 0.5f + swing, 48);
  draw->PathStroke(ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)), 0, 2.0f);

  const float bobRadius = std::max(10.0f, height * 0.075f);
  const ImVec4 accent = theme::Accent();

  draw->AddLine(pivot, bob, ImGui::GetColorU32(running ? theme::Control() : theme::TextFaint()),
                std::max(2.5f, height * 0.008f));

  // A soft ring around the bob on the beat, so the downbeat is visible from across the room even
  // when the arm is at the edge of your vision.
  if (running)
  {
    const float pulse = (1.0f - phase) * (1.0f - phase);
    draw->AddCircleFilled(bob, bobRadius * (1.0f + pulse * 0.9f),
                          ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, pulse * 0.22f)), 32);
  }

  draw->AddCircleFilled(bob, bobRadius, ImGui::GetColorU32(running ? accent : theme::TextFaint()), 32);

  // The mount it hangs from.
  draw->AddCircleFilled(pivot, std::max(4.0f, height * 0.014f), ImGui::GetColorU32(theme::TextDim()), 16);

  ImGui::Dummy(ImVec2(width, height));
}

void MainWindow::DrawBeatFlash(float width, float height, float phase, int beat, bool running)
{
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 area = ImVec2(origin.x + width, origin.y + height);

  // Full at the instant of the beat, gone before the next one. Squared so it drops away quickly
  // and then lingers faintly, which reads as a flash rather than a fade.
  const float decay = running ? (1.0f - phase) * (1.0f - phase) : 0.0f;
  const ImVec4 colour = (beat == 0) ? theme::Accent() : theme::Control();

  // The flash is the only fill. Nothing behind it, so between beats the area is black rather than
  // a panel waiting to be flashed.
  draw->AddRectFilled(origin, area, ImGui::GetColorU32(ImVec4(colour.x, colour.y, colour.z, decay * 0.16f)), 8.0f);

  // The bar laid out as dots, with the current beat lit. Tells you where you are, which a bare
  // flash cannot - and sized to the space it has been given rather than to a fixed maximum, so a
  // tall window gets a big display instead of a small one in a large empty field.
  const int beats = std::max(1, mMetronome.beatsPerBar);
  const float spacing = std::min(width / static_cast<float>(beats + 1), height * 0.42f);
  const float radius = std::min(spacing * 0.34f, height * 0.17f);
  const float startX = origin.x + width * 0.5f - spacing * static_cast<float>(beats - 1) * 0.5f;

  for (int i = 0; i < beats; i++)
  {
    const ImVec2 at(startX + spacing * static_cast<float>(i), origin.y + height * 0.5f);
    const bool current = running && (i == beat);
    const ImVec4 dot = (i == 0) ? theme::Accent() : theme::Control();

    if (current)
    {
      // A halo that swells on the beat and shrinks back, so the eye catches it in peripheral
      // vision rather than having to be looking straight at the dot.
      draw->AddCircleFilled(at, radius * (1.0f + decay * 0.7f),
                            ImGui::GetColorU32(ImVec4(dot.x, dot.y, dot.z, 0.22f)), 40);
      draw->AddCircleFilled(at, radius, ImGui::GetColorU32(dot), 40);
    }
    else
    {
      draw->AddCircle(at, radius, ImGui::GetColorU32(ImVec4(dot.x, dot.y, dot.z, 0.28f)), 40,
                      std::max(2.0f, radius * 0.09f));
    }
  }

  ImGui::Dummy(ImVec2(width, height));
}

void MainWindow::DrawMetronomeView()
{
  // Nothing here scrolls. A metronome is a single panel of settings around one big readout, and a
  // scrollbar down the side of it means the readout can be pushed off the screen you are watching.
  // Everything fits instead, by giving the visual whatever the controls leave.
  ImGui::BeginChild("##metronome", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  const float phase = mEngine.GetMetronomePhase();
  const int beat = mEngine.GetMetronomeBeat();

  // --- the preset, at the top ---
  //
  // The name of what is loaded belongs where a document's name goes - the top left, above the thing
  // it describes - the same as the project name in the Play view. At the foot of the panel it read
  // as the last setting rather than as the title of everything above it.
  DrawMetronomePresetMenu();

  // --- the visual ---
  //
  // It is the reason this view exists - the thing you glance at from across the room while playing -
  // so it takes the whole of what the controls do not need, rather than a fixed share.
  const float visualHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - mMetronomeControlsHeight);
  const float visualWidth = ImGui::GetContentRegionAvail().x;

  if (mMetronomeVisual == 1)
    DrawBeatFlash(visualWidth, visualHeight, phase, beat, mMetronomeRunning);
  else
    DrawPendulum(visualWidth, visualHeight, phase, beat, mMetronomeRunning);

  // Everything from here down is the control block whose height the visual was sized against.
  const float controlsTop = ImGui::GetCursorPosY();

  if (SegmentedButton("PENDULUM", mMetronomeVisual == 0, 120.0f) && mMetronomeVisual != 0)
  {
    mMetronomeVisual = 0;
    ApplyMetronome();
    mConfig.Save();
  }
  ImGui::SameLine(0, 4);
  if (SegmentedButton("FLASH", mMetronomeVisual == 1, 120.0f) && mMetronomeVisual != 1)
  {
    mMetronomeVisual = 1;
    ApplyMetronome();
    mConfig.Save();
  }

  ImGui::SameLine(0, 24);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint());
  ImGui::Text("bar %d   beat %d of %d", mEngine.GetMetronomeBar() + 1, beat + 1, mMetronome.beatsPerBar);
  ImGui::PopStyleColor();

  theme::Divider();

  // --- the tempo, under the thing it drives ---
  //
  // The number and the transport used to sit above the visual, which put the controls between the
  // window's edge and the one thing on this screen you look at from a distance. Underneath, the
  // reading is at the top and the setting of it is where a hand goes.
  //
  // The big number is the field: a tempo is something you often know exactly, and a slider you
  // have to nudge onto 138 is no way to enter it.
  int bpm = static_cast<int>(std::lround(mMetronome.bpm));
  theme::PushHeading(2.2f);
  ImGui::PushStyleColor(ImGuiCol_Text, mMetronomeRunning ? theme::Accent() : theme::Control());
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0)); // no box until you reach for it
  ImGui::SetNextItemWidth(130.0f);
  const bool typed = ImGui::InputInt("##bpmfield", &bpm, 0, 0, ImGuiInputTextFlags_CharsDecimal);
  ImGui::PopStyleColor(2);
  theme::PopFont();

  if (typed)
  {
    mMetronome.bpm = static_cast<float>(std::clamp(bpm, 20, 300));
    ApplyMetronome();
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();

  ImGui::SameLine(0, 6);
  theme::Label("BPM");

  // The same key the transport uses, because it is the same action: start the thing that keeps
  // time. Two different shapes for start would say they were two different kinds of start.
  ImGui::SameLine(0, 22);
  const ImVec2 keySize(46.0f, 36.0f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);
  if (theme::TransportKey("##metrostart", mMetronomeRunning ? theme::Icon::Stop : theme::Icon::Play, keySize,
                          mMetronomeRunning))
  {
    mMetronomeRunning = !mMetronomeRunning;
    ApplyMetronome();
    mConfig.Save();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(mMetronomeRunning ? "Stop the click" : "Start the click");

  // Tap tempo: four taps is enough to be sure, and taps more than two seconds apart are a new
  // attempt rather than a very slow tempo.
  ImGui::SameLine(0, 8);
  if (ImGui::Button("TAP", ImVec2(74, keySize.y)))
  {
    const double now = ImGui::GetTime();
    if (!mTapTimes.empty() && now - mTapTimes.back() > 2.0)
      mTapTimes.clear();
    mTapTimes.push_back(now);
    if (mTapTimes.size() > 5)
      mTapTimes.erase(mTapTimes.begin());

    if (mTapTimes.size() >= 2)
    {
      const double span = mTapTimes.back() - mTapTimes.front();
      const double perBeat = span / static_cast<double>(mTapTimes.size() - 1);
      if (perBeat > 0.0)
      {
        mMetronome.bpm = std::clamp(static_cast<float>(60.0 / perBeat), 20.0f, 300.0f);
        ApplyMetronome();
      }
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Tap four times in time with the music");

  ImGui::SameLine(0, 22);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
  if (theme::SlimSlider("##bpm", &mMetronome.bpm, 20.0f, 300.0f, 120.0f,
                        std::max(120.0f, ImGui::GetContentRegionAvail().x - 8.0f), "%.0f BPM"))
    ApplyMetronome();
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();

  // --- time signature and subdivision ---
  //
  // One label column down the left and one control column beside it, the way a track's row reads
  // in the Play view. Everything that is a value on a line is the same slim slider there, so the
  // beat count is one here too rather than a drag field with a box around it.
  theme::SectionLabel("TIME");

  constexpr float kLabelColumn = 132.0f;
  constexpr float kSliderWidth = 260.0f;

  theme::LabelFor("Beats per bar", kLabelColumn);
  float beats = static_cast<float>(mMetronome.beatsPerBar);
  if (theme::SlimSlider("##beats", &beats, 1.0f, 12.0f, 4.0f, 180.0f, "%.0f"))
  {
    mMetronome.beatsPerBar = std::clamp(static_cast<int>(std::lround(beats)), 1, 12);
    ApplyMetronome();
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();
  ImGui::SameLine(0, 14);
  theme::Value(std::to_string(mMetronome.beatsPerBar).c_str(), 1.0f);

  theme::LabelFor("Beat unit", kLabelColumn);
  static const int kUnits[] = {2, 4, 8, 16};
  for (int unit : kUnits)
  {
    char label[8];
    std::snprintf(label, sizeof(label), "%d", unit);
    if (SegmentedButton(label, mMetronome.beatUnit == unit, 44.0f) && mMetronome.beatUnit != unit)
    {
      mMetronome.beatUnit = unit;
      ApplyMetronome();
      mConfig.Save();
    }
    ImGui::SameLine(0, 4);
  }
  ImGui::NewLine();

  theme::LabelFor("Subdivision", kLabelColumn);
  static const char* kSubNames[] = {"OFF", "8THS", "TRIPLETS", "16THS"};
  static const int kSubValues[] = {1, 2, 3, 4};
  for (int i = 0; i < 4; i++)
  {
    if (SegmentedButton(kSubNames[i], mMetronome.subdivision == kSubValues[i], 92.0f)
        && mMetronome.subdivision != kSubValues[i])
    {
      mMetronome.subdivision = kSubValues[i];
      ApplyMetronome();
      mConfig.Save();
    }
    ImGui::SameLine(0, 4);
  }
  ImGui::NewLine();

  theme::Hint("BPM counts the beat unit, so 6/8 at 120 is 120 eighths a minute.");

  // --- sounds ---
  //
  // Three voices as three keys rather than a dropdown: there are only three, and hearing the
  // difference means trying them, which a menu makes into two clicks each time.
  theme::SectionLabel("SOUNDS");

  const auto soundRow = [&](const char* label, int* voice, float* levelDb, bool* enabled)
  {
    ImGui::PushID(label);

    theme::LabelFor(label, kLabelColumn);

    static const char* kVoiceNames[] = {"BEEP", "WOOD", "TICK"};
    for (int i = 0; i < 3; i++)
    {
      if (SegmentedButton(kVoiceNames[i], *voice == i, 64.0f) && *voice != i)
      {
        *voice = i;
        ApplyMetronome();
        mConfig.Save();
      }
      ImGui::SameLine(0, 4);
    }

    ImGui::SameLine(0, 18);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
    if (theme::SlimSlider("##level", levelDb, -40.0f, 6.0f, 0.0f, kSliderWidth, "%+.1f dB"))
      ApplyMetronome();
    if (ImGui::IsItemDeactivatedAfterEdit())
      mConfig.Save();

    // The one row that can be switched off carries the switch at its end, where the eye has
    // already read what it applies to.
    if (enabled != nullptr)
    {
      ImGui::SameLine(0, 16);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6.0f);
      if (theme::IconButton("##on", theme::Icon::Volume, 26.0f, theme::IconStyle::Bare, *enabled))
      {
        *enabled = !*enabled;
        ApplyMetronome();
        mConfig.Save();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(*enabled ? "Subdivision on" : "Subdivision off");
    }

    ImGui::PopID();
  };

  soundRow("Accent", &mMetronome.accentVoice, &mMetronome.accentDb, nullptr);
  soundRow("Beat", &mMetronome.beatVoice, &mMetronome.beatDb, nullptr);
  soundRow("Subdivision", &mMetronome.subVoice, &mMetronome.subDb, &mMetronome.subEnabled);

  ImGui::Spacing();
  theme::LabelFor("Metronome level", kLabelColumn);
  if (theme::SlimSlider("##metlevel", &mMetronome.levelDb, -40.0f, 6.0f, 0.0f, kSliderWidth, "%+.1f dB"))
    ApplyMetronome();
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();

  mMetronomeControlsHeight = ImGui::GetCursorPosY() - controlsTop;

  ImGui::EndChild();
}

/// \brief The preset the metronome is on, and everything you can do to it.
///
/// The same shape the project name has in the Play view: the name of what is loaded, a caret, and
/// the actions behind it. The list used to be laid out flat at the foot of the view - a text field,
/// a Save button and a stack of rows - which gave saving and deleting more of the screen than the
/// tempo they belong to, and grew the panel a line every time a preset was added.
void MainWindow::DrawMetronomePresetMenu()
{
  const bool named = mMetronomePresetName[0] != '\0';
  std::string label = named ? std::string(mMetronomePresetName) : std::string("No preset");
  label += "###metropreset";

  ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (theme::MenuButton(label.c_str(), 1.2f, 240.0f))
    ImGui::OpenPopup("##presetmenu");

  if (ImGui::BeginPopup("##presetmenu"))
  {
    if (mConfig.metronomePresets.empty())
      theme::Hint("Nothing saved yet. Set a tempo you want back and save it under a name.");

    for (size_t i = 0; i < mConfig.metronomePresets.size(); i++)
    {
      const AppConfig::MetronomePreset& preset = mConfig.metronomePresets[i];
      ImGui::PushID(static_cast<int>(i));

      // The tempo and signature ride along as the shortcut column, the way a menu shows a key
      // binding: it is what tells two presets apart before you have learned the names.
      char detail[32];
      std::snprintf(detail, sizeof(detail), "%.0f  %d/%d", preset.bpm, preset.beatsPerBar, preset.beatUnit);
      const bool current = named && preset.name == mMetronomePresetName;

      if (ImGui::MenuItem(preset.name.c_str(), detail, current))
      {
        const std::string keptName = preset.name;
        mMetronome = preset;
        std::snprintf(mMetronomePresetName, sizeof(mMetronomePresetName), "%s", keptName.c_str());
        ApplyMetronome();
        mConfig.Save();
        ImGui::PopID();
        break;
      }

      ImGui::PopID();
    }

    ImGui::Separator();

    // Naming and saving live at the foot of the menu, so the list above stays a list. Enter saves,
    // which is what a field with a button beside it should always do.
    ImGui::SetNextItemWidth(-1.0f);
    const bool entered = ImGui::InputTextWithHint("##presetname", "Preset name", mMetronomePresetName,
                                                  sizeof(mMetronomePresetName), ImGuiInputTextFlags_EnterReturnsTrue);

    const bool canSave = mMetronomePresetName[0] != '\0';
    ImGui::BeginDisabled(!canSave);
    const bool pressed = ImGui::MenuItem("Save", "Enter");
    ImGui::EndDisabled();

    if ((entered || pressed) && canSave)
    {
      AppConfig::MetronomePreset preset = mMetronome;
      preset.name = mMetronomePresetName;

      // Saving over a name replaces it, rather than leaving two entries that look the same.
      bool replaced = false;
      for (auto& existing : mConfig.metronomePresets)
        if (existing.name == preset.name)
        {
          existing = preset;
          replaced = true;
        }
      if (!replaced)
        mConfig.metronomePresets.push_back(preset);
      mConfig.Save();
      ImGui::CloseCurrentPopup();
    }

    // Delete only ever applies to the one that is loaded, which is the one whose name is on the
    // button. A row of little crosses down the list is four ways to lose a preset by accident.
    bool exists = false;
    for (const auto& preset : mConfig.metronomePresets)
      exists = exists || (named && preset.name == mMetronomePresetName);

    ImGui::BeginDisabled(!exists);
    if (ImGui::MenuItem("Delete"))
    {
      for (size_t i = 0; i < mConfig.metronomePresets.size(); i++)
        if (mConfig.metronomePresets[i].name == mMetronomePresetName)
        {
          mConfig.metronomePresets.erase(mConfig.metronomePresets.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
      mMetronomePresetName[0] = '\0';
      mConfig.Save();
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
  }
}

void MainWindow::DrawTunerView()
{
  Tuner& tuner = mEngine.GetTuner();

  // Follows the string being played, unless one has been locked. Done before anything is read, so
  // the whole frame is drawn against the same note.
  FollowDetectedNote();

  const double referenceHz = NoteFrequency(mTunerNoteIndex, mTunerOctave, static_cast<double>(mTunerA4Hz));
  const float cents = tuner.GetCentsError();
  const bool hasSignal = tuner.HasSignal();
  // Matches the width of the needle's green band, so "green" means the same thing in both displays.
  // The strobe is the one that resolves finer than this; that is what it is for.
  const bool inTune = hasSignal && std::fabs(cents) < 3.0f;

  ImGui::BeginChild("##tuner", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);

  // --- hearing yourself, out of the way of everything else ---
  //
  // A speaker in the top corner, on its own. It is not part of tuning - it is whether the amp is
  // audible while you do it - and as a labelled row among the tunings it read like one.
  {
    constexpr float kSpeaker = 34.0f;
    const float back = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kSpeaker;
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX(back);
    if (theme::IconButton("##tunermute", theme::Icon::Volume, kSpeaker, theme::IconStyle::Bare, !mTunerMutesOutput))
    {
      mTunerMutesOutput = !mTunerMutesOutput;
      mConfig.tunerMutesOutput = mTunerMutesOutput;
      mConfig.Save();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(mTunerMutesOutput ? "Muted while you tune. Click to hear yourself"
                                          : "Playing through. Click to mute while you tune");
    ImGui::SetCursorPosY(top);
  }

  // --- the display, one way or the other ---
  //
  // First, and given about half the height, like the metronome's pendulum and for the same reason:
  // this is what you read while both hands are on the instrument. The name of the note used to sit
  // above it, which put a line of text between the window's edge and the thing being watched.
  const float displayHeight = std::max(180.0f, ImGui::GetContentRegionAvail().y * 0.46f);

  if (mTunerNeedleMode)
  {
    // A needle has mass. Without something standing in for it the pointer is redrawn at a new
    // angle every frame, and sixty of those a second read as a fan of lines rather than as one
    // needle. Smoothed against wall-clock time, so it moves the same on any machine.
    const float follow = 1.0f - std::exp(-ImGui::GetIO().DeltaTime / 0.10f);
    mNeedleCents += ((hasSignal ? cents : 0.0f) - mNeedleCents) * follow;

    // And a tolerance at dead centre, so a string that is in tune settles instead of hunting
    // around zero. Narrower than the green band, so the needle is always inside it when it stops.
    constexpr float kSettleCents = 1.5f;
    const float shown = std::fabs(mNeedleCents) < kSettleCents ? 0.0f : mNeedleCents;

    // The gauge is a half-disc, so its width follows its height rather than the window's: stretched
    // to a wide window it becomes an ellipse, and the needle no longer sweeps evenly.
    const float gaugeWidth = std::min(ImGui::GetContentRegionAvail().x, displayHeight / 0.52f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - gaugeWidth) * 0.5f);
    theme::NeedleGauge(shown, hasSignal, inTune, gaugeWidth, displayHeight);
  }
  else
  {
    // Three bands, slowest first. The fundamental tells you which way to turn the peg; the octave
    // and double octave crawl to a stop only when you are genuinely there.
    static const char* kBandLabels[kStrobeBandCount] = {"fundamental", "octave", "two octaves"};
    const float stripWidth = ImGui::GetContentRegionAvail().x - 150.0f;
    const float stripHeight = (displayHeight - ImGui::GetStyle().ItemSpacing.y * 2.0f) / kStrobeBandCount;
    for (size_t band = 0; band < kStrobeBandCount; band++)
    {
      const float top = ImGui::GetCursorPosY();
      theme::StrobeStrip(tuner.GetBandPhase(band), hasSignal ? tuner.GetBandStrength(band) : 0.0f, stripWidth,
                         stripHeight, 16 * static_cast<int>(band + 1), inTune);
      ImGui::SameLine(0, 14);
      // Centred against the strip rather than sitting on its top edge.
      ImGui::SetCursorPosY(top + (stripHeight - ImGui::GetTextLineHeight() * 2.0f) * 0.5f);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::Text("%s\n%.1f Hz", kBandLabels[band], referenceHz * static_cast<double>(1 << band));
      ImGui::PopStyleColor();
    }
  }

  // --- the reading, under the thing it is a reading of ---
  //
  // The note, as large as the panel allows: readable from across a room, which is where you are
  // when you are holding the instrument. Grey until something is being played, because in auto the
  // name is a reading, and a reading with no signal behind it is a guess.
  ImGui::Spacing();
  theme::PushHeading(3.2f);
  ImGui::PushStyleColor(ImGuiCol_Text,
                        !hasSignal ? theme::TextFaint() : (inTune ? theme::Success() : theme::Accent()));
  ImGui::TextUnformatted(NoteName(mTunerNoteIndex, mTunerOctave).c_str());
  ImGui::PopStyleColor();
  theme::PopFont();

  // The reference frequency, and how it was arrived at. A cents figure next to a strobe or a
  // needle would be a third reading of the same thing, and the only one not readable at a glance.
  ImGui::SameLine(0, 24);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::Text("%.2f Hz", referenceHz);
  ImGui::PopStyleColor();

  // Which of the two it is, and the way out of the one that is a mode. Auto needs no button of its
  // own: it is what happens when you have not asked for anything.
  ImGui::SameLine(0, 14);
  if (mTunerAuto)
  {
    theme::Chip("auto", theme::TextDim());
  }
  else
  {
    if (ImGui::SmallButton("locked  x"))
      SetTunerAuto(true);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Holding %s. Click to follow what you play again",
                        NoteName(mTunerNoteIndex, mTunerOctave).c_str());
  }

  // The choice of display goes on the far right of the same row - it is a setting, and it was
  // taking a line of its own directly under the thing it switches.
  {
    constexpr float kModeWidth = 100.0f;
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kModeWidth * 2.0f - 4.0f);
    if (SegmentedButton("STROBE", !mTunerNeedleMode, kModeWidth))
    {
      mTunerNeedleMode = false;
      mConfig.tunerNeedleMode = false;
      mConfig.Save();
    }
    ImGui::SameLine(0, 4);
    if (SegmentedButton("NEEDLE", mTunerNeedleMode, kModeWidth))
    {
      mTunerNeedleMode = true;
      mConfig.tunerNeedleMode = true;
      mConfig.Save();
    }
  }

  theme::Divider();

  // --- which instrument, and how it is tuned ---
  //
  // No heading. Four labelled rows under a rule are plainly a group already, and "TUNING" over the
  // top of them said only what the words in them say.
  constexpr float kTunerLabel = 118.0f;

  const auto saveTuning = [&]()
  {
    mConfig.tunerInstrument = mTunerInstrument;
    mConfig.tunerStringCount = mTunerStringCount;
    mConfig.tunerTuningName = mTunerTuningName;
    mConfig.Save();
  };

  // Anything that changes the set of strings drops a custom string with it: the overrides are
  // written against positions in a tuning, and they mean nothing once it is a different one.
  const auto moveTo = [&](int instrument, int stringCount, const std::string& name)
  {
    mTunerInstrument = instrument;
    mTunerStringCount = stringCount;
    mTunerTuningName = name;
    mTunerCustomStrings = false;
    mConfig.tunerCustomStrings.clear();
    saveTuning();
  };

  theme::LabelFor("Instrument", kTunerLabel);
  static const char* kInstrumentNames[] = {"GUITAR", "BASS"};
  for (int instrument = 0; instrument < 2; instrument++)
  {
    if (instrument > 0)
      ImGui::SameLine(0, 4);
    if (SegmentedButton(kInstrumentNames[instrument], mTunerInstrument == instrument, 92)
        && mTunerInstrument != instrument)
    {
      // The string count and tuning almost certainly do not exist for the new instrument, so fall
      // back to its first rather than leaving a combination that matches nothing.
      for (const auto& preset : Tunings())
        if (preset.instrument == instrument)
        {
          moveTo(instrument, preset.stringCount, preset.Name());
          break;
        }
    }
  }

  // String count as buttons rather than a dropdown: it is one of five values, you know which one
  // you own, and having it on screen is what keeps the rows below it short.
  theme::LabelFor("Strings", kTunerLabel);
  bool firstCount = true;
  for (int count = 4; count <= 8; count++)
  {
    const TuningPreset* first = FirstTuning(mTunerInstrument, count);
    if (first == nullptr)
      continue;

    if (!firstCount)
      ImGui::SameLine(0, 4);
    firstCount = false;

    char label[8];
    std::snprintf(label, sizeof(label), "%d", count);
    if (SegmentedButton(label, mTunerStringCount == count, 42) && mTunerStringCount != count)
      moveTo(mTunerInstrument, count, first->Name());
  }

  // --- the tuning, as the two things it actually is ---
  //
  // A family and a key. One list of every combination meant reading past "Eb standard" and "Drop C"
  // to find "D standard", when what you know before you look is that you play in drop and you want
  // it in C.
  const TuningPreset* active = FindTuning(mTunerInstrument, mTunerStringCount, mTunerTuningName);
  if (active == nullptr)
  {
    // A name from an older config, or a combination that no longer exists.
    active = FirstTuning(mTunerInstrument, mTunerStringCount);
    if (active != nullptr)
    {
      mTunerTuningName = active->Name();
      mTunerCustomStrings = false;
      saveTuning();
    }
  }

  if (active != nullptr)
  {
    theme::LabelFor("Type", kTunerLabel);
    std::string lastType;
    bool firstType = true;
    for (const auto& preset : Tunings())
    {
      if (preset.instrument != mTunerInstrument || preset.stringCount != mTunerStringCount
          || preset.type == lastType)
        continue;
      lastType = preset.type;

      if (!firstType)
        ImGui::SameLine(0, 4);
      firstType = false;

      if (SegmentedButton(preset.type, std::string(active->type) == preset.type, 108)
          && std::string(active->type) != preset.type)
      {
        // Keep the key if the new family has one by that name - moving from standard E to drop
        // should land on drop E where it exists rather than on whatever comes first.
        const TuningPreset* wanted = nullptr;
        for (const auto& candidate : Tunings())
          if (candidate.instrument == mTunerInstrument && candidate.stringCount == mTunerStringCount
              && std::string(candidate.type) == preset.type)
          {
            if (wanted == nullptr || std::string(candidate.key) == active->key)
              wanted = &candidate;
            if (std::string(candidate.key) == active->key)
              break;
          }
        if (wanted != nullptr)
          moveTo(mTunerInstrument, mTunerStringCount, wanted->Name());
      }
    }

    theme::LabelFor("Key", kTunerLabel);
    bool firstKey = true;
    for (const auto& preset : Tunings())
    {
      if (preset.instrument != mTunerInstrument || preset.stringCount != mTunerStringCount
          || std::string(preset.type) != active->type)
        continue;

      if (!firstKey)
        ImGui::SameLine(0, 4);
      firstKey = false;

      if (SegmentedButton(preset.key, std::string(active->key) == preset.key, 52)
          && std::string(active->key) != preset.key)
        moveTo(mTunerInstrument, mTunerStringCount, preset.Name());
    }

    // --- the strings ---
    //
    // In auto these are a readout: the one you are playing lights up. A press holds it, a press on
    // the lit one lets go again, and a press held down opens the note it stands for - which is how
    // a tuning nobody wrote down gets entered, and why there is no "other note" row any more.
    theme::LabelFor("String", kTunerLabel);
    TunedString strings[8];
    EffectiveStrings(*active, mTunerCustomStrings, mTunerStringMidi.data(), strings);

    // Editing one string starts from wherever the tuning has them, so a custom tuning is the
    // preset with a change on top rather than eight notes to enter from scratch.
    const auto setString = [&](int index, int note, int octave)
    {
      if (!mTunerCustomStrings)
      {
        TunedString from[8];
        EffectiveStrings(*active, false, nullptr, from);
        for (int s = 0; s < active->stringCount; s++)
          mTunerStringMidi[static_cast<size_t>(s)] = (from[s].octave + 1) * 12 + from[s].noteIndex;
        mTunerCustomStrings = true;
      }
      mTunerStringMidi[static_cast<size_t>(index)] = (octave + 1) * 12 + note;

      mConfig.tunerCustomStrings.assign(mTunerStringMidi.begin(),
                                        mTunerStringMidi.begin() + active->stringCount);
      mConfig.Save();
    };

    for (int i = 0; i < active->stringCount; i++)
    {
      if (i > 0)
        ImGui::SameLine(0, 4);
      ImGui::PushID(i);

      const TunedString string = strings[i];
      const bool on = (mTunerNoteIndex == string.noteIndex && mTunerOctave == string.octave);
      const bool clicked = SegmentedButton(NoteName(string.noteIndex, string.octave).c_str(), on, 56);

      // Held rather than right-clicked: this is a control you use with one hand while the other is
      // on the neck, and a hold is the same gesture on a trackpad as on a mouse.
      if (ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] > 0.4f && mTunerEditingString < 0)
      {
        mTunerEditingString = i;
        ImGui::OpenPopup("##stringnote");
      }

      if (mTunerEditingString == i && ImGui::BeginPopup("##stringnote"))
      {
        theme::Hint("What this string is tuned to.");
        for (int octave = 0; octave <= 6; octave++)
        {
          ImGui::PushID(octave);
          for (int note = 0; note < 12; note++)
          {
            if (note > 0)
              ImGui::SameLine(0, 2);
            const bool here = (note == string.noteIndex && octave == string.octave);
            if (SegmentedButton(NoteName(note, octave).c_str(), here, 44))
            {
              setString(i, note, octave);
              mTunerNoteIndex = note;
              mTunerOctave = octave;
              mConfig.tunerNoteIndex = note;
              mConfig.tunerOctave = octave;
              SetTunerAuto(false);
              ImGui::CloseCurrentPopup();
            }
          }
          ImGui::PopID();
        }
        ImGui::EndPopup();
      }
      else if (mTunerEditingString == i && !ImGui::IsPopupOpen("##stringnote"))
      {
        mTunerEditingString = -1;
      }

      // A click that was really the end of a hold is not a click.
      if (clicked && mTunerEditingString != i)
      {
        if (on && !mTunerAuto)
          SetTunerAuto(true);
        else
        {
          mTunerNoteIndex = string.noteIndex;
          mTunerOctave = string.octave;
          mConfig.tunerNoteIndex = mTunerNoteIndex;
          mConfig.tunerOctave = mTunerOctave;
          SetTunerAuto(false);
        }
      }

      ImGui::PopID();
    }

    if (mTunerCustomStrings)
    {
      ImGui::SameLine(0, 14);
      if (ImGui::SmallButton("reset"))
      {
        mTunerCustomStrings = false;
        mConfig.tunerCustomStrings.clear();
        mConfig.Save();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to %s", active->Name().c_str());
    }
  }

  theme::SectionLabel("REFERENCE");

  theme::LabelFor("Concert pitch", kTunerLabel);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
  if (theme::SlimSlider("##a4", &mTunerA4Hz, 415.0f, 466.0f, 440.0f, 260.0f, "%.1f Hz"))
  {
    // Settles onto concert pitch as it passes. Everything else on this slider is a deliberate
    // choice; 440 is the one value you have to be able to get back to exactly, by hand, every time.
    if (std::fabs(mTunerA4Hz - 440.0f) < 1.2f)
      mTunerA4Hz = 440.0f;
    mConfig.tunerA4Hz = mTunerA4Hz;
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();
  ImGui::SameLine(0, 14);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);
  char a4[16];
  std::snprintf(a4, sizeof(a4), "%.1f Hz", mTunerA4Hz);
  theme::Value(a4, 1.0f);

  ImGui::EndChild();

  UpdateTunerReference();
}


void MainWindow::DrawSettingsView()
{
  // Two columns rather than one long one: settings read badly stretched across a wide window, and
  // worse still as a narrow ribbon with the rest of the screen left empty.
  // Fills what it is given. The transport's row is already taken out one level up.
  const float bodyHeight = 0.0f;
  const float columnWidth =
    std::max(320.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

  ImGui::BeginChild("##setaudio", ImVec2(columnWidth, bodyHeight), true);

  theme::SectionLabel("AUDIO DEVICE");

  // Labels in a column of their own rather than to the right of each control. ImGui puts a widget's
  // label after it, so a full-width combo pushed its own name off the edge - which is why these
  // rows read as unlabelled boxes.
  constexpr float kSettingLabel = 118.0f;

  theme::LabelFor("API", kSettingLabel);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##api",
                        RtAudio::getApiDisplayName(mCompiledApis[static_cast<size_t>(mSelectedApiIndex)]).c_str()))
  {
    for (size_t i = 0; i < mCompiledApis.size(); i++)
    {
      if (ImGui::Selectable(RtAudio::getApiDisplayName(mCompiledApis[i]).c_str(),
                            static_cast<int>(i) == mSelectedApiIndex))
      {
        mSelectedApiIndex = static_cast<int>(i);
        // Release the device before probing: a still-open stream on another API can make a
        // driver that wants exclusive access (ASIO) report no devices at all.
        mEngine.Close();
        RefreshDeviceLists();
      }
    }
    ImGui::EndCombo();
  }

  theme::LabelFor("Input", kSettingLabel);
  ImGui::SetNextItemWidth(-1);
  const char* inputPreview = (mSelectedInputIndex >= 0)
                               ? mInputDevices[static_cast<size_t>(mSelectedInputIndex)].name.c_str()
                               : "(no input device)";
  if (ImGui::BeginCombo("##input", inputPreview))
  {
    for (size_t i = 0; i < mInputDevices.size(); i++)
      if (ImGui::Selectable(mInputDevices[i].name.c_str(), static_cast<int>(i) == mSelectedInputIndex))
        mSelectedInputIndex = static_cast<int>(i);
    ImGui::EndCombo();
  }

  theme::LabelFor("Output", kSettingLabel);
  ImGui::SetNextItemWidth(-1);
  const char* outputPreview = (mSelectedOutputIndex >= 0)
                                ? mOutputDevices[static_cast<size_t>(mSelectedOutputIndex)].name.c_str()
                                : "(no output device)";
  if (ImGui::BeginCombo("##output", outputPreview))
  {
    for (size_t i = 0; i < mOutputDevices.size(); i++)
      if (ImGui::Selectable(mOutputDevices[i].name.c_str(), static_cast<int>(i) == mSelectedOutputIndex))
        mSelectedOutputIndex = static_cast<int>(i);
    ImGui::EndCombo();
  }

  char sampleRatePreview[32];
  std::snprintf(sampleRatePreview, sizeof(sampleRatePreview), "%u Hz",
                kCommonSampleRates[static_cast<size_t>(mSelectedSampleRateIndex)]);
  theme::LabelFor("Sample rate", kSettingLabel);
  ImGui::SetNextItemWidth(150);
  if (ImGui::BeginCombo("##rate", sampleRatePreview))
  {
    for (size_t i = 0; i < std::size(kCommonSampleRates); i++)
    {
      char label[32];
      std::snprintf(label, sizeof(label), "%u Hz", kCommonSampleRates[i]);
      if (ImGui::Selectable(label, static_cast<int>(i) == mSelectedSampleRateIndex))
        mSelectedSampleRateIndex = static_cast<int>(i);
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine(0, 16);
  theme::Label("Buffer");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110);
  ImGui::InputInt("##buffer", &mBufferFramesValue, 0);
  mBufferFramesValue = std::clamp(mBufferFramesValue, 16, 8192);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextUnformatted("(?)");
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("ASIO drivers usually dictate the buffer size themselves.\n"
                      "If this has no effect, change it in your interface's own control panel.");

  if (ImGui::Button("Apply and open", ImVec2(200, 0)))
    ApplyDeviceSelection();

  if (!mEngine.GetLastError().empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    ImGui::TextWrapped("%s", mEngine.GetLastError().c_str());
    ImGui::PopStyleColor();
  }
  if (mInputDevices.empty() && mOutputDevices.empty() && !mEngine.GetLastProbeMessage().empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
    ImGui::TextWrapped("No devices for this API: %s", mEngine.GetLastProbeMessage().c_str());
    ImGui::PopStyleColor();
  }

  ImGui::EndChild();

  // --- everything that is not the audio device ---
  ImGui::SameLine();
  ImGui::BeginChild("##setrest", ImVec2(0, bodyHeight), true);

  theme::SectionLabel("LEVELS");

  if (theme::Check("Match capture loudness", &mAutoNormalize))
  {
    mConfig.autoNormalize = mAutoNormalize;
    mConfig.Save();
    mLastMatchedCapture.clear(); // so the next frame re-applies it to whatever is loaded now
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Captures carry the loudness they were trained at. With this on, switching "
                     "capture sets the output level to compensate, so browsing does not change how "
                     "loud you are. The OUT knob stays yours either way.");
  ImGui::PopStyleColor();

  theme::SectionLabel("STEM SEPARATION");

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Splitting a song into drums, bass, other and vocals is done by Demucs, which "
                     "runs as a separate program. Install it with \"pip install demucs\" and leave "
                     "the command below as it is, or point at it directly.");
  ImGui::PopStyleColor();

  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##demucs", "demucs   (leave empty to find it automatically)", mDemucsCommand,
                               sizeof(mDemucsCommand)))
    mConfig.demucsCommand = mDemucsCommand;
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();

  // Only what belongs to this machine lives here. Which model, how it splits and what it runs on
  // are settings for a particular run, and they are asked for when you start one.
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  const std::filesystem::path stemsRoot = mConfig.stemsFolder.empty()
                                            ? AppConfig::GetConfigFolder() / "stems"
                                            : std::filesystem::path(mConfig.stemsFolder);
  ImGui::TextWrapped("Stems are written to %s", stemsRoot.string().c_str());
  ImGui::PopStyleColor();

  theme::SectionLabel("CHORD TRANSCRIPTION");

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Working out several notes at once is done by basic-pitch, which is a Python "
                     "program: \"pip install basic-pitch\". Leave this empty unless Python is not on "
                     "PATH, or unless it is installed for a different one than the one that answers.");
  ImGui::PopStyleColor();

  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##transcribe", "python   (leave empty to find it automatically)", mTranscribeCommand,
                               sizeof(mTranscribeCommand)))
    mConfig.transcribeCommand = mTranscribeCommand;
  if (ImGui::IsItemDeactivatedAfterEdit())
    mConfig.Save();

  // Said out loud, because "it found one" and "it will try PATH and probably fail" look identical
  // until you press the button.
  const std::string python = TranscribePython();
  std::error_code pythonEc;
  const bool managed = python == ManagedPythonPath().string();

  ImGui::PushStyleColor(ImGuiCol_Text, python.empty() ? theme::Warning() : theme::TextDim());
  if (python.empty())
    ImGui::TextWrapped("Using: whatever Python is on PATH. TensorFlow's Windows builds stop at Python "
                       "3.11, so a newer one will not be able to install basic-pitch.");
  else
    ImGui::TextWrapped("Using: %s%s", python.c_str(), managed ? "  (set up beside this app)" : "  (your setting)");
  ImGui::PopStyleColor();

  (void)pythonEc;

  theme::SectionLabel("CAPTURE FOLDER");

  const auto folder = mCaptureLibrary.GetFolder();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("%s", folder.empty() ? "No folder selected" : folder.string().c_str());
  ImGui::TextWrapped("Scanned for .nam captures and .wav impulse responses, subfolders included. "
                     "Downloads are filed into a subfolder per tone, with their tags kept alongside.");
  ImGui::PopStyleColor();

  if (ImGui::Button("Choose folder", ImVec2(200, 0)))
  {
    HWND hwnd = glfwGetWin32Window(mWindow);
    auto picked = BrowseForFolder(hwnd, L"Choose a folder with .nam captures", folder);
    if (picked.has_value())
    {
      mCaptureLibrary.SetFolder(*picked);
      mCaptureLibrary.SetUserTags(mConfig.userTags);
      mConfig.captureFolder = picked->string();
      mConfig.Save();
    }
  }

  theme::SectionLabel("TONE3000 ACCOUNT");

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("Sign in with your TONE3000 username and password in your browser. The app "
                     "never sees your password, and you can disconnect it from your TONE3000 "
                     "account at any time.");
  ImGui::PopStyleColor();

  ImGui::Spacing();

  if (mTone3000.IsAuthorizing())
  {
    theme::Spinner(ImGui::GetFrameHeight());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted("Finish signing in in your browser");
    ImGui::PopStyleColor();
  }
  else if (mTone3000.IsConnected())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Success());
    ImGui::TextUnformatted("Connected");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Reconnect", ImVec2(140, 0)))
      mTone3000.BeginAuthorizationAsync();
    ImGui::SameLine();
    if (ImGui::Button("Disconnect", ImVec2(140, 0)))
    {
      mTone3000.Disconnect();
      mConfig.tone3000RefreshTokenEncrypted.clear();
      mConfig.Save();
      mCloudListsLoaded = false;
    }
  }
  else
  {
    ImGui::BeginDisabled(!mTone3000.HasClientId());
    if (ImGui::Button("Sign in to TONE3000", ImVec2(260, 0)))
      mTone3000.BeginAuthorizationAsync();
    ImGui::EndDisabled();

    // Only a build with no key of its own gets to explain itself. For everyone else this whole
    // paragraph, and the section below it, may as well not exist.
    if (!mTone3000.HasClientId())
    {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
      ImGui::TextWrapped("This build has no TONE3000 application key, so signing in is not "
                         "available. See Advanced below.");
      ImGui::PopStyleColor();
    }
  }

  if (!mTone3000.GetStatusMessage().empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped("%s", mTone3000.GetStatusMessage().c_str());
    ImGui::PopStyleColor();
  }

  // Everything below is for someone building the app themselves. A player signing in has no
  // business knowing that any of it exists, so it stays folded away.
  if (ImGui::CollapsingHeader("Advanced"))
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped("The publishable key identifies this application to TONE3000 - it is not "
                       "your account and not a password. Builds normally carry their own; set one "
                       "here only if this build has none, or to use your own.");
    ImGui::PopStyleColor();

    const auto applyClientId = [&]()
    {
      mConfig.tone3000ClientId = mClientIdInput;
      mTone3000.SetClientId(mConfig.tone3000ClientId.empty() ? std::string(kTone3000AppClientId)
                                                             : mConfig.tone3000ClientId);
    };

    ImGui::SetNextItemWidth(-150);
    if (ImGui::InputTextWithHint("##clientid", "Publishable key", mClientIdInput, sizeof(mClientIdInput)))
      applyClientId();
    if (ImGui::IsItemDeactivatedAfterEdit())
      mConfig.Save();

    // A key left in this box wins over the one the build carries, which is right for a deliberate
    // override and a trap for anything typed here once and forgotten. This is the way back.
    ImGui::SameLine();
    ImGui::BeginDisabled(mConfig.tone3000ClientId.empty());
    if (ImGui::Button("Use built-in", ImVec2(-1, 0)))
    {
      mClientIdInput[0] = '\0';
      applyClientId();
      mConfig.Save();
    }
    ImGui::EndDisabled();

    // What is actually being sent, which is not always what you think you typed. "Unknown
    // client_id" from the server is otherwise a dead end to diagnose.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    const std::string inUse = mTone3000.GetClientId();
    ImGui::TextWrapped("Sending: %s%s", inUse.empty() ? "(none)" : inUse.c_str(),
                       mConfig.tone3000ClientId.empty() ? "   (built in)" : "   (override)");
    ImGui::PopStyleColor();

    // Shown to be copied rather than described, because it has to match to the character if
    // TONE3000 turns out to require it to be registered.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("Redirect URI, if your account requires registering one");
    ImGui::PopStyleColor();

    char redirect[128];
    std::snprintf(redirect, sizeof(redirect), "%s", OAuthRedirectUri().c_str());
    ImGui::SetNextItemWidth(-110);
    ImGui::InputText("##redirect", redirect, sizeof(redirect), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Copy", ImVec2(100, 0)))
      ImGui::SetClipboardText(redirect);
  }

  // What this is, and what it is built on, live behind the mark at the head of the rail now. Two
  // places saying the same thing is one place too many, and the mark is where a person looks.

  theme::SectionLabel("KEYBOARD");
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextUnformatted("1            select the gate");
  ImGui::TextUnformatted("2, 3, 4 ...  select a block, left to right along the chain");
  ImGui::TextUnformatted("Space        take the selected unit in or out of the chain");
  ImGui::TextUnformatted("Up / Down    move through the library and audition as you go");
  ImGui::TextUnformatted("Enter        load the highlighted file into the selected block");
  ImGui::TextUnformatted("Delete       remove the selected block, after confirming");
  ImGui::TextUnformatted("F11          fullscreen");
  ImGui::PopStyleColor();

  // No Close button: this is a place you navigate away from, like the other views, not a dialog
  // you dismiss.
  ImGui::EndChild();
}

void MainWindow::DrawRigList()
{
  // Listed once and cached: this is a directory scan, and it only changes when this app changes it.
  if (!mRigNamesLoaded)
  {
    mRigNames = RigFile::List();
    mRigNamesLoaded = true;
  }

  theme::PushHeading(1.05f);
  ImGui::TextUnformatted("Rigs");
  theme::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextWrapped("A rig is the whole chain - every block, what is loaded into it, the routing "
                     "and the gate. Not the audio device or the input and output levels: those "
                     "belong to where you are playing rather than to the sound.");
  ImGui::PopStyleColor();

  // Saving happens under the rig's name at the top of the Rig view, where the thing being saved
  // is. A second name field down here was a second place to keep track of.
  ImGui::Separator();

  if (mRigNames.empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("No rigs saved yet.");
    ImGui::PopStyleColor();
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("##rigs", 2, kFlags))
    return;

  ImGui::TableSetupColumn("Rig", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 90.0f);

  for (const auto& name : mRigNames)
  {
    ImGui::TableNextRow();
    ImGui::PushID(name.c_str());

    ImGui::TableSetColumnIndex(0);
    const bool isCurrent = (name == mCurrentRigName);
    // Double click rather than single: loading a rig replaces everything you are playing, which is
    // not something a stray click should do.
    if (ImGui::Selectable(name.c_str(), isCurrent, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
    {
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        LoadRig(name);
    }
    if (ImGui::BeginPopupContextItem("##rigmenu"))
    {
      if (ImGui::MenuItem("Load"))
        LoadRig(name);
      if (ImGui::MenuItem("Overwrite with current rig"))
        SaveRig(name);
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
      if (ImGui::MenuItem("Delete"))
        mPendingDeleteRig = name;
      ImGui::PopStyleColor();
      ImGui::EndPopup();
    }

    if (isCurrent)
    {
      ImGui::SameLine();
      theme::Chip("loaded", theme::Accent());
    }

    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Load", ImVec2(-1, 0)))
      LoadRig(name);

    ImGui::PopID();
  }
  ImGui::EndTable();

}

/// \brief The rig the chain is on, and what you can do to it.
///
/// The same control the project name and the metronome preset use: the name of what is loaded, a
/// caret, and the actions behind it. A rig could be saved and loaded only from the library panel
/// before, which meant the thing the whole view is an edit of was named somewhere else entirely.
void MainWindow::DrawRigMenu()
{
  if (!mRigNamesLoaded)
  {
    mRigNames = RigFile::List();
    mRigNamesLoaded = true;
  }

  const bool named = !mCurrentRigName.empty();
  const std::string label = (named ? mCurrentRigName : std::string("Unsaved rig")) + "###rigmenu";

  // The chain's switch, at the far end of the row that names the chain. It used to sit in the top
  // corner of the rail, a large orange ring alone in a field of black with nothing to say what it
  // belonged to.
  {
    constexpr float kPower = 30.0f;
    const float back = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kPower;
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(back, top + 2.0f));

    bool chainActive = !mEngine.IsBypassed();
    if (theme::PowerButton("bypass", &chainActive, kPower))
    {
      mEngine.SetBypassed(!chainActive);
      mConfig.bypassed = !chainActive;
      mConfig.Save();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(chainActive ? "Chain active" : "Chain bypassed");

    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, top));
  }

  ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (theme::MenuButton(label.c_str(), 1.2f, 240.0f))
  {
    // Saving defaults to the loaded rig's name, so re-saving is one press and saving a variation
    // is one edit.
    if (mRigNameInput[0] == '\0' && named)
      std::snprintf(mRigNameInput, sizeof(mRigNameInput), "%s", mCurrentRigName.c_str());
    ImGui::OpenPopup("##rigsmenu");
  }

  if (!ImGui::BeginPopup("##rigsmenu"))
    return;

  if (mRigNames.empty())
    theme::Hint("Nothing saved yet. A rig is the whole chain - every block, what is loaded into "
                "it, the routing and the gate.");

  for (const auto& name : mRigNames)
  {
    if (ImGui::MenuItem(name.c_str(), nullptr, name == mCurrentRigName))
    {
      LoadRig(name);
      std::snprintf(mRigNameInput, sizeof(mRigNameInput), "%s", name.c_str());
      break;
    }
  }

  ImGui::Separator();

  ImGui::SetNextItemWidth(-1.0f);
  const bool entered = ImGui::InputTextWithHint("##rigname", "Rig name", mRigNameInput, sizeof(mRigNameInput),
                                                ImGuiInputTextFlags_EnterReturnsTrue);

  const bool canSave = mRigNameInput[0] != '\0';
  ImGui::BeginDisabled(!canSave);
  const bool pressed = ImGui::MenuItem("Save", "Enter");
  ImGui::EndDisabled();

  if ((entered || pressed) && canSave)
  {
    SaveRig(mRigNameInput);
    ImGui::CloseCurrentPopup();
  }

  ImGui::BeginDisabled(!named);
  if (ImGui::MenuItem("Delete"))
    mPendingDeleteRig = mCurrentRigName;
  ImGui::EndDisabled();

  ImGui::EndPopup();
}

/// Deleting a rig is not undoable, so it asks - same as removing a block or a capture. Drawn from
/// the top level, because it can be asked for from the rig's own menu or from the library panel,
/// and a modal owned by one of them would never open from the other.
void MainWindow::DrawDeleteRigPopup()
{
  if (mPendingDeleteRig.empty())
    return;

  if (!ImGui::IsPopupOpen("Delete rig"))
    ImGui::OpenPopup("Delete rig");

  if (ImGui::BeginPopupModal("Delete rig", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Delete the rig \"%s\"?", mPendingDeleteRig.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("The captures and IRs it used are not touched.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    const bool confirmed = ImGui::Button("Delete", ImVec2(140, 0));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel", ImVec2(140, 0));

    if (confirmed)
    {
      RigFile::Remove(mPendingDeleteRig);
      if (mCurrentRigName == mPendingDeleteRig)
        mCurrentRigName.clear();
      mRigNamesLoaded = false;
      mPendingDeleteRig.clear();
      ImGui::CloseCurrentPopup();
    }
    else if (cancelled)
    {
      mPendingDeleteRig.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void MainWindow::DrawBlockTargetSelector()
{
  const auto blocks = mEngine.GetBlocks();

  // Where a click in the list sends the file. A combo rather than a row of buttons: it is a
  // destination you set occasionally, and a button per block would take over a toolbar whose job
  // is searching.
  std::string current = "no block";
  for (size_t index = 0; index < blocks.size(); index++)
    if (blocks[index].id == mSelectedBlockId)
      current = std::to_string(index + 1) + " " + BlockTypeName(blocks[index].settings.type);

  const std::string preview = "Load into  " + current;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##loadinto", preview.c_str()))
  {
    for (size_t index = 0; index < blocks.size(); index++)
    {
      const Block& block = blocks[index];
      ImGui::PushID(block.id);

      // What is in a block already is the thing that tells you whether you want to overwrite it.
      std::string label = std::to_string(index + 1) + "  " + BlockTypeName(block.settings.type);
      const auto loaded = mBlockPaths.find(block.id);
      if (loaded != mBlockPaths.end() && !loaded->second.empty())
        label += "   " + loaded->second.stem().string();

      if (ImGui::Selectable(label.c_str(), block.id == mSelectedBlockId))
        mSelectedBlockId = block.id;
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
}

void MainWindow::DrawLibraryView()
{
  // Fills what it is given. The transport's row is already taken out one level up.
  const float bodyHeight = 0.0f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;

  // Sources as tabs across the top: local and TONE3000 are the same activity, so they belong in
  // one place rather than behind separate destinations.
  static const char* kSourceNames[] = {"Rigs", "All", "Favorites", "Recent", "TONE3000"};
  if (ImGui::BeginTabBar("##librarysources"))
  {
    for (int i = 0; i < IM_ARRAYSIZE(kSourceNames); i++)
    {
      // The cloud tab is tinted apart from the local ones: it is the one that reaches the network
      // and downloads files, which is worth signalling before you click it.
      const bool isCloud = (i == static_cast<int>(LibrarySource::Cloud));
      if (isCloud)
      {
        const ImVec4 cloud = BlockColor(BlockType::Ir);
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(cloud.x, cloud.y, cloud.z, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(cloud.x, cloud.y, cloud.z, 0.38f));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(cloud.x, cloud.y, cloud.z, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, cloud);
        ImGui::PushStyleColor(ImGuiCol_Text, cloud);
      }

      if (ImGui::BeginTabItem(kSourceNames[i]))
      {
        mSource = static_cast<LibrarySource>(i);
        ImGui::EndTabItem();
      }

      if (isCloud)
        ImGui::PopStyleColor(5);
    }
    ImGui::EndTabBar();
  }

  if (mSource == LibrarySource::Cloud && !mCloudListsLoaded && mTone3000.IsConnected())
  {
    mTone3000.FetchTagsAsync("");
    mTone3000.FetchMakesAsync("");
    mTone3000.FetchCreatorsAsync("");
    mCloudListsLoaded = true;
  }

  // Rigs are whole setups rather than files, so they have no filters and no per-file details to
  // show. The tab gets the window to itself.
  if (mSource == LibrarySource::Rigs)
  {
    ImGui::BeginChild("##riglist", ImVec2(0, bodyHeight), true);
    DrawRigList();
    ImGui::EndChild();
    return;
  }

  // The filter column is a share of the window rather than a fixed width, so it stays usable on a
  // laptop screen and does not become a canyon on a wide one.
  const float filterWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.20f, 185.0f, 290.0f);

  // Details sit on the right, the same shape for a local file and a search result. The pane is
  // always there rather than appearing on selection - a column that comes and goes reflows the
  // list underneath it every time you click.
  const float detailWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.22f, 210.0f, 320.0f);

  ImGui::BeginChild("##libfilters", ImVec2(filterWidth, bodyHeight), true);
  DrawLibraryFilters();
  ImGui::EndChild();

  ImGui::SameLine(0, spacing);

  const float listWidth = std::max(240.0f, ImGui::GetContentRegionAvail().x - detailWidth - spacing);
  ImGui::BeginChild("##liblist", ImVec2(listWidth, bodyHeight), true);
  // TONE3000 is a source inside the library, not a separate destination.
  if (mSource == LibrarySource::Cloud)
    DrawCloudResults();
  else
    DrawLibraryList();
  ImGui::EndChild();

  ImGui::SameLine(0, spacing);

  ImGui::BeginChild("##libdetails", ImVec2(0, bodyHeight), true);
  if (mSource == LibrarySource::Cloud)
    DrawToneDetails();
  else
    DrawCaptureDetails();
  ImGui::EndChild();

  // Opened outside the pane it was triggered from, which disappears with the file.
  DrawRemoveCapturePopup();

  // Filter changes coalesce into one request, issued as soon as the client is free. Without this,
  // toggling several filters quickly would have most of the requests silently dropped.
  if (mSearchPending && !mTone3000.IsSearching())
  {
    mFilters.query = mSearchQuery;
    mTone3000.SearchAsync(mFilters, 1);
    mSearchPending = false;
  }
}

bool MainWindow::DrawFilterSection(const char* label, const std::vector<std::string>& values,
                                  std::vector<std::string>& target, char* searchBuffer, size_t searchSize)
{
  if (values.empty())
    return false;

  // Collapsed unless the user opened it, or asked for everything to open. A column of four sections
  // standing open is a column you scroll past rather than read.
  if (mFilterExpandRequest != 0)
    ImGui::SetNextItemOpen(mFilterExpandRequest > 0);

  // How many are ticked, on the header itself: the one thing worth knowing about a closed section.
  std::string heading = label;
  if (!target.empty())
    heading += "  (" + std::to_string(target.size()) + ")";
  heading += "###";
  heading += label;

  if (!ImGui::CollapsingHeader(heading.c_str()))
    return false;

  bool changed = false;
  ImGui::PushID(label);

  // Only worth a search box once the list is long enough that scanning it is work.
  const bool searchable = values.size() > 8;
  if (searchable)
  {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search", searchBuffer, searchSize);
  }
  const std::string needle = ToLower(std::string(searchBuffer));

  // Values arrive alphabetically from the library; a ticked value stays visible even when the
  // search would hide it, so nothing is filtering invisibly.
  for (const auto& value : values)
  {
    const bool selected = VectorContains(target, value);
    if (!selected && !needle.empty() && ToLower(value).find(needle) == std::string::npos)
      continue;

    ImGui::PushID(value.c_str());
    changed |= FilterCheckbox(value.c_str(), target, value);
    ImGui::PopID();
  }

  ImGui::PopID();
  return changed;
}

bool MainWindow::DrawFilterSummary(const std::vector<std::vector<std::string>*>& groups)
{
  size_t total = 0;
  for (const auto* group : groups)
    total += group->size();
  if (total == 0)
    return false;

  bool changed = false;
  const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

  // Each active value as a button that drops it. Wrapped by hand, because these are buttons rather
  // than text and nothing wraps them for us.
  for (auto* group : groups)
  {
    for (size_t i = 0; i < group->size();)
    {
      const std::string label = (*group)[i] + "  x";
      const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
      if (i > 0 || group != groups.front())
      {
        ImGui::SameLine(0, 4);
        if (ImGui::GetCursorPosX() + width > right)
          ImGui::NewLine();
      }

      ImGui::PushID(&(*group)[i]);
      const bool dropped = ImGui::SmallButton(label.c_str());
      ImGui::PopID();

      if (dropped)
      {
        group->erase(group->begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
      }
      else
      {
        i++;
      }
    }
  }

  ImGui::Spacing();
  return changed;
}

void MainWindow::DrawLibraryFilters()
{
  // The cloud has its own filter set and draws its own heading, so hand the column over whole
  // rather than drawing a second "Filters" title and Clear button above it.
  if (mSource == LibrarySource::Cloud)
  {
    DrawCloudFilters();
    return;
  }

  theme::PushHeading(1.05f);
  ImGui::TextUnformatted("Filters");
  theme::PopFont();

  if (ImGui::SmallButton("Expand all"))
    mFilterExpandRequest = 1;
  ImGui::SameLine(0, 4);
  if (ImGui::SmallButton("Collapse all"))
    mFilterExpandRequest = -1;
  ImGui::SameLine(0, 4);
  if (ImGui::SmallButton("Clear"))
  {
    mLocalTagFilter.clear();
    mLocalGearFilter.clear();
    mLocalMakeFilter.clear();
    mLocalCreatorFilter.clear();
  }
  ImGui::Separator();

  DrawFilterSummary({&mLocalTagFilter, &mLocalGearFilter, &mLocalMakeFilter, &mLocalCreatorFilter});

  // Every list below is built from the sidecar metadata written at download time, plus whatever
  // tags you added yourself, so the categories a capture had on TONE3000 keep working offline.
  DrawFilterSection("Tags", mCaptureLibrary.GetKnownTags(), mLocalTagFilter, mLocalTagSearch, sizeof(mLocalTagSearch));
  DrawFilterSection("Gear", mCaptureLibrary.GetKnownGear(), mLocalGearFilter, mLocalGearSearch,
                    sizeof(mLocalGearSearch));
  DrawFilterSection("Makes and models", mCaptureLibrary.GetKnownMakes(), mLocalMakeFilter, mLocalMakeSearch,
                    sizeof(mLocalMakeSearch));
  DrawFilterSection("Creators", mCaptureLibrary.GetKnownCreators(), mLocalCreatorFilter, mLocalCreatorSearch,
                    sizeof(mLocalCreatorSearch));

  mFilterExpandRequest = 0;

  if (mCaptureLibrary.GetKnownTags().empty())
  {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped("Captures downloaded through this app carry their TONE3000 tags, which show "
                       "up here as filters. Files added by hand have no metadata to filter on.");
    ImGui::PopStyleColor();
  }
}

void MainWindow::DrawCaptureActions(const CaptureEntry& entry, bool asMenu)
{
  const std::string pathString = entry.path.string();
  const bool isFavorite = mConfig.IsFavorite(pathString);

  // A capture only fits blocks of its own kind, so the list offers exactly those - loading an IR
  // into a NAM block would just switch the block's type behind your back.
  const auto blocks = mEngine.GetBlocks();
  const BlockType wanted = entry.isIr ? BlockType::Ir : BlockType::Nam;

  const auto loadInto = [&](const Block& block, size_t index)
  {
    std::string label = std::to_string(index + 1) + "  " + BlockTypeName(block.settings.type);
    const auto loaded = mBlockPaths.find(block.id);
    if (loaded != mBlockPaths.end() && !loaded->second.empty())
      label += "   " + loaded->second.stem().string();

    const bool clicked = asMenu ? ImGui::MenuItem(label.c_str()) : ImGui::Button(label.c_str(), ImVec2(-1, 0));
    if (clicked)
    {
      mSelectedBlockId = block.id;
      LoadFileIntoBlock(block.id, entry.path);
    }
  };

  if (asMenu)
  {
    if (ImGui::BeginMenu("Load into"))
    {
      for (size_t i = 0; i < blocks.size(); i++)
        if (blocks[i].settings.type == wanted)
          loadInto(blocks[i], i);
      ImGui::Separator();
      if (ImGui::MenuItem("New block"))
      {
        const int id = AddBlock(wanted);
        if (id > 0)
          LoadFileIntoBlock(id, entry.path);
      }
      ImGui::EndMenu();
    }
  }
  else
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("LOAD INTO");
    ImGui::PopStyleColor();
    for (size_t i = 0; i < blocks.size(); i++)
      if (blocks[i].settings.type == wanted)
        loadInto(blocks[i], i);
    if (ImGui::Button("New block", ImVec2(-1, 0)))
    {
      const int id = AddBlock(wanted);
      if (id > 0)
        LoadFileIntoBlock(id, entry.path);
    }
    ImGui::Spacing();
  }

  const char* favoriteLabel = isFavorite ? "Remove from favourites" : "Add to favourites";
  const bool favoriteClicked =
    asMenu ? ImGui::MenuItem(favoriteLabel) : ImGui::Button(favoriteLabel, ImVec2(-1, 0));
  if (favoriteClicked)
  {
    mConfig.ToggleFavorite(pathString);
    mConfig.Save();
  }

  if (asMenu)
    ImGui::Separator();

  ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
  const bool deleteClicked =
    asMenu ? ImGui::MenuItem("Delete file...") : ImGui::Button("Delete file...", ImVec2(-1, 0));
  ImGui::PopStyleColor();
  if (deleteClicked)
    mPendingDeletePath = entry.path;
}

void MainWindow::DrawRemoveCapturePopup()
{
  if (mPendingDeletePath.empty())
    return;

  if (!ImGui::IsPopupOpen("Delete capture"))
    ImGui::OpenPopup("Delete capture");

  if (ImGui::BeginPopupModal("Delete capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Delete %s from disk?", mPendingDeletePath.filename().string().c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("This removes the file itself, not just the library entry.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    const bool confirmed = ImGui::Button("Delete", ImVec2(140, 0));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel", ImVec2(140, 0));

    if (confirmed)
    {
      const std::string pathString = mPendingDeletePath.string();
      std::error_code ec;
      std::filesystem::remove(mPendingDeletePath, ec);

      // Everything that pointed at the file goes with it, or the next scan resurrects it as a
      // favourite that no longer exists.
      if (mConfig.IsFavorite(pathString))
        mConfig.ToggleFavorite(pathString);
      mConfig.recent.erase(std::remove(mConfig.recent.begin(), mConfig.recent.end(), pathString),
                           mConfig.recent.end());
      mConfig.userTags.erase(pathString);
      mConfig.Save();

      if (mSelectedCapturePath == mPendingDeletePath)
        mSelectedCapturePath.clear();

      mCaptureLibrary.Refresh();
      mCaptureLibrary.SetUserTags(mConfig.userTags);
      mPendingDeletePath.clear();
      ImGui::CloseCurrentPopup();
    }
    else if (cancelled)
    {
      mPendingDeletePath.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void MainWindow::DrawCaptureDetails()
{
  // Resolved by path rather than held as a pointer: the entry vector is rebuilt on every rescan.
  const CaptureEntry* entry = nullptr;
  for (const auto& candidate : mCaptureLibrary.GetEntries())
    if (candidate.path == mSelectedCapturePath)
      entry = &candidate;

  if (entry == nullptr)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped("Select a capture to see its details.");
    ImGui::PopStyleColor();
    return;
  }

  theme::PushHeading(1.05f);
  ImGui::TextWrapped("%s", entry->displayName.c_str());
  theme::PopFont();

  theme::Chip(entry->isIr ? "IR" : "NAM", BlockColor(entry->isIr ? BlockType::Ir : BlockType::Nam));
  ImGui::Separator();

  const auto field = [](const char* label, const std::string& value)
  {
    if (value.empty())
      return;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(110.0f);
    ImGui::TextWrapped("%s", value.c_str());
  };

  field("Folder", entry->groupName);
  field("Creator", entry->meta.creator);
  field("Gear", entry->meta.gear);
  field("Architecture", entry->meta.architecture);
  field("License", entry->meta.license);

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextUnformatted("TAGS");
  ImGui::PopStyleColor();

  // What came with the capture is not editable; what you added is. Two colours say which is which
  // without a word of explanation.
  const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  bool first = true;
  const auto tagRow = [&](float width)
  {
    if (!first)
    {
      ImGui::SameLine(0, 4);
      if (ImGui::GetCursorPosX() + width > right)
        ImGui::NewLine();
    }
    first = false;
  };

  for (const auto& tag : entry->meta.tags)
  {
    tagRow(theme::ChipWidth(tag.c_str()));
    theme::Chip(tag.c_str(), theme::TextDim());
  }

  const std::string pathString = entry->path.string();
  auto& mine = mConfig.userTags[pathString];
  for (size_t i = 0; i < mine.size();)
  {
    const std::string label = mine[i] + "  x";
    tagRow(ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);

    ImGui::PushID(static_cast<int>(i));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    const bool dropped = ImGui::SmallButton(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopID();

    if (dropped)
    {
      mine.erase(mine.begin() + static_cast<std::ptrdiff_t>(i));
      mConfig.Save();
      mCaptureLibrary.SetUserTags(mConfig.userTags);
    }
    else
    {
      i++;
    }
  }
  if (first)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("none");
    ImGui::PopStyleColor();
  }

  ImGui::Spacing();
  constexpr float kAddWidth = 52.0f;
  ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - kAddWidth
                                            - ImGui::GetStyle().ItemSpacing.x));
  const bool entered = ImGui::InputTextWithHint("##newtag", "Add tag", mNewTagText, sizeof(mNewTagText),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const bool added = ImGui::Button("Add", ImVec2(-1, 0));

  if ((entered || added) && mNewTagText[0] != '\0')
  {
    const std::string tag = mNewTagText;
    if (!VectorContains(mine, tag) && !VectorContains(entry->meta.tags, tag))
    {
      mine.push_back(tag);
      mConfig.Save();
      mCaptureLibrary.SetUserTags(mConfig.userTags);
    }
    mNewTagText[0] = '\0';
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  DrawCaptureActions(*entry, false);
}

void MainWindow::DrawLibraryList()
{
  // --- toolbar: what you are looking for, and where a click sends it ---
  constexpr float kTargetWidth = 190.0f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - kTargetWidth - spacing));
  ImGui::InputTextWithHint("##capturefilter", "Search name, tag, make, creator", mCaptureFilterText,
                           sizeof(mCaptureFilterText));
  ImGui::SameLine();
  DrawBlockTargetSelector();

  if (ImGui::SmallButton("Expand all"))
    mGroupExpandRequest = 1;
  ImGui::SameLine(0, 4);
  if (ImGui::SmallButton("Collapse all"))
    mGroupExpandRequest = -1;

  const std::string filter = ToLower(std::string(mCaptureFilterText));
  const auto& entries = mCaptureLibrary.GetEntries();

  const auto matches = [&](const CaptureEntry& entry)
  {
    if (!filter.empty() && entry.searchHaystack.find(filter) == std::string::npos)
      return false;
    // Within a category the selections are AND-ed: each ticked tag must be present. A tag you
    // added counts the same as one the capture arrived with.
    for (const auto& required : mLocalTagFilter)
      if (!VectorContains(entry.meta.tags, required) && !VectorContains(entry.userTags, required))
        return false;
    for (const auto& required : mLocalMakeFilter)
      if (!VectorContains(entry.meta.makes, required))
        return false;
    // Gear and creator are single-valued, so a selection there is a set of allowed values.
    if (!mLocalGearFilter.empty() && !VectorContains(mLocalGearFilter, entry.meta.gear))
      return false;
    if (!mLocalCreatorFilter.empty() && !VectorContains(mLocalCreatorFilter, entry.meta.creator))
      return false;
    if (mSource == LibrarySource::Favorites && !mConfig.IsFavorite(entry.path.string()))
      return false;
    return true;
  };

  // Build the visible set for this frame; keyboard navigation walks exactly this list.
  mVisiblePaths.clear();
  std::vector<const CaptureEntry*> visible;

  if (mSource == LibrarySource::Recent)
  {
    // Recent keeps its own order (most recent first), so walk that rather than the folder.
    for (const auto& recentPath : mConfig.recent)
      for (const auto& entry : entries)
        if (entry.path.string() == recentPath && matches(entry))
          visible.push_back(&entry);
  }
  else
  {
    for (const auto& entry : entries)
      if (matches(entry))
        visible.push_back(&entry);
  }

  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  if (mCaptureLibrary.GetFolder().empty())
    ImGui::TextUnformatted("No capture folder yet - open Settings");
  else if (visible.size() == entries.size())
    ImGui::Text("%d files", static_cast<int>(entries.size()));
  else
    ImGui::Text("%d of %d", static_cast<int>(visible.size()), static_cast<int>(entries.size()));
  ImGui::PopStyleColor();

  // Which block, if any, each file is currently sitting in - so the chain is readable from here.
  const auto chainBlocks = mEngine.GetBlocks();

  // A table rather than one Selectable per row with the metadata glued into its label. Columns
  // line up down the page, which is the whole difference between a list you scan and one you read.
  // Tristate sorting: a column cycles ascending, descending, off. "Off" is a state worth having
  // here, because it is the one that keeps the files grouped by the folder they were downloaded
  // into - a sort has to cut across those folders to mean anything.
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX
                                     | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
  if (!ImGui::BeginTable("##captures", 5, kFlags))
    return;

  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.42f, kLibColumnName);
  ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 46.0f, kLibColumnType);
  ImGui::TableSetupColumn("Creator", ImGuiTableColumnFlags_WidthStretch, 0.22f, kLibColumnCreator);
  ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.36f);
  ImGui::TableSetupColumn("##fav", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 24.0f);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  // Sorting flattens the list: rows from different folders interleave, so the folder headings
  // would be meaningless. Each row shows its folder instead.
  bool sorted = false;
  if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
  {
    if (specs->SpecsCount > 0)
    {
      sorted = true;
      const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
      const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

      std::stable_sort(visible.begin(), visible.end(),
                       [&](const CaptureEntry* a, const CaptureEntry* b)
                       {
                         int order = 0;
                         switch (spec.ColumnUserID)
                         {
                           case kLibColumnType:
                             order = static_cast<int>(a->isIr) - static_cast<int>(b->isIr);
                             break;
                           case kLibColumnCreator:
                             order = CompareNoCase(a->meta.creator, b->meta.creator);
                             break;
                           default: break;
                         }
                         // Name is both the default key and the tie-break, so equal creators or
                         // types come out alphabetical rather than in scan order.
                         if (order == 0)
                           order = CompareNoCase(a->displayName, b->displayName);
                         return ascending ? order < 0 : order > 0;
                       });
    }
  }

  // Filled after the sort, not before: keyboard navigation has to walk the list in the order it is
  // actually drawn in, or the arrow keys jump around the screen.
  for (const auto* entry : visible)
    mVisiblePaths.push_back(entry->path);
  mSelectedIndex = std::clamp(mSelectedIndex, 0, std::max(0, static_cast<int>(mVisiblePaths.size()) - 1));

  std::string currentGroup = "\x01"; // sentinel: no group printed yet
  bool groupOpen = true;
  bool groupPushed = false; // a tree node is open and still owes a TreePop

  for (int i = 0; i < static_cast<int>(visible.size()); i++)
  {
    const CaptureEntry& entry = *visible[static_cast<size_t>(i)];
    const std::string pathString = entry.path.string();
    const bool isFavorite = mConfig.IsFavorite(pathString);

    // Downloads land in one folder per tone, so each folder collapses as a unit - a pack of forty
    // gain stages is one line until you want to see inside it.
    if (!sorted && mSource != LibrarySource::Recent && entry.groupName != currentGroup)
    {
      if (groupPushed)
      {
        ImGui::TreePop();
        groupPushed = false;
      }
      currentGroup = entry.groupName;
      if (currentGroup.empty())
      {
        groupOpen = true;
      }
      else
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        // Collapsed by default: a folder is one line until you ask for its forty gain stages.
        if (mGroupExpandRequest != 0)
          ImGui::SetNextItemOpen(mGroupExpandRequest > 0);

        // How many files are in there, since the folder is closed most of the time.
        size_t inGroup = 0;
        for (size_t look = static_cast<size_t>(i); look < visible.size(); look++)
        {
          if (visible[look]->groupName != currentGroup)
            break;
          inGroup++;
        }

        const std::string heading = currentGroup + "   (" + std::to_string(inGroup) + ")###" + currentGroup;
        groupOpen = ImGui::TreeNodeEx(heading.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
        groupPushed = groupOpen;

        // The union of what is inside, so a closed folder still says what it holds. Every file in
        // a downloaded pack shares one sidecar, so without deduplicating this would be the same
        // handful of tags repeated forty times.
        std::vector<std::string> folderTags;
        for (size_t look = static_cast<size_t>(i); look < static_cast<size_t>(i) + inGroup; look++)
        {
          for (const auto& tag : visible[look]->meta.tags)
            if (!VectorContains(folderTags, tag))
              folderTags.push_back(tag);
          for (const auto& tag : visible[look]->userTags)
            if (!VectorContains(folderTags, tag))
              folderTags.push_back(tag);
        }

        ImGui::TableSetColumnIndex(3);
        float folderBudget = ImGui::GetContentRegionAvail().x;
        for (size_t t = 0; t < folderTags.size(); t++)
        {
          const float width = theme::ChipWidth(folderTags[t].c_str()) + (t > 0 ? 4.0f : 0.0f);
          if (width > folderBudget)
            break;
          if (t > 0)
            ImGui::SameLine(0, 4);
          theme::Chip(folderTags[t].c_str(), theme::TextDim());
          folderBudget -= width;
        }
      }
    }
    if (!groupOpen)
      continue;

    ImGui::TableNextRow();
    ImGui::PushID(i);

    // --- name, and a chip for every block this file is currently loaded into ---
    ImGui::TableSetColumnIndex(0);
    const bool isSelected = (entry.path == mSelectedCapturePath);
    if (ImGui::Selectable(entry.displayName.c_str(), isSelected,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
    {
      mSelectedIndex = i;
      mSelectedCapturePath = entry.path;
      LoadLibraryEntry(entry);
    }

    // Right-click selects too, so the menu and the details pane always agree about what you are
    // acting on.
    if (ImGui::BeginPopupContextItem("##capturemenu"))
    {
      mSelectedIndex = i;
      mSelectedCapturePath = entry.path;
      DrawCaptureActions(entry, true);
      ImGui::EndPopup();
    }

    // With the folder headings gone, the folder still has to be readable somewhere.
    if (sorted && !entry.groupName.empty())
    {
      ImGui::SameLine(0, 8);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::TextUnformatted(entry.groupName.c_str());
      ImGui::PopStyleColor();
    }

    for (size_t blockIndex = 0; blockIndex < chainBlocks.size(); blockIndex++)
    {
      const auto loaded = mBlockPaths.find(chainBlocks[blockIndex].id);
      if (loaded == mBlockPaths.end() || loaded->second != entry.path)
        continue;
      ImGui::SameLine(0, 6);
      theme::Chip(std::to_string(blockIndex + 1).c_str(), theme::Accent());
    }

    ImGui::TableSetColumnIndex(1);
    theme::Chip(entry.isIr ? "IR" : "NAM", BlockColor(entry.isIr ? BlockType::Ir : BlockType::Nam));

    ImGui::TableSetColumnIndex(2);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted(entry.meta.creator.empty() ? "-" : entry.meta.creator.c_str());
    ImGui::PopStyleColor();

    // Tags inline, as many as fit. They were only reachable through a tooltip before, one row at
    // a time, which is no way to compare forty captures.
    // Yours first and in the accent colour: they are the ones you chose, so they are the ones
    // worth seeing when only a couple fit.
    ImGui::TableSetColumnIndex(3);
    float budget = ImGui::GetContentRegionAvail().x;
    bool firstTag = true;
    const auto tagChip = [&](const std::string& tag, ImVec4 color)
    {
      const float width = theme::ChipWidth(tag.c_str()) + (firstTag ? 0.0f : 4.0f);
      if (width > budget)
        return false;
      if (!firstTag)
        ImGui::SameLine(0, 4);
      theme::Chip(tag.c_str(), color);
      budget -= width;
      firstTag = false;
      return true;
    };

    for (const auto& tag : entry.userTags)
      if (!tagChip(tag, theme::Accent()))
        break;
    for (const auto& tag : entry.meta.tags)
      if (!tagChip(tag, theme::TextDim()))
        break;

    // Filled when it is a favorite, hollow when it is not.
    ImGui::TableSetColumnIndex(4);
    if (theme::StarButton("fav", isFavorite, 20.0f))
    {
      mConfig.ToggleFavorite(pathString);
      mConfig.Save();
    }

    ImGui::PopID();
  }

  if (groupPushed)
    ImGui::TreePop();
  ImGui::EndTable();

  mGroupExpandRequest = 0;
}

void MainWindow::DrawCloudResults()
{
  const bool searching = mTone3000.IsSearching();
  const bool noFolder = mCaptureLibrary.GetFolder().empty();

  constexpr float kSearchButton = 88.0f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - kSearchButton - spacing));
  if (ImGui::InputTextWithHint("##search", "Search TONE3000", mSearchQuery, sizeof(mSearchQuery),
                               ImGuiInputTextFlags_EnterReturnsTrue))
    mSearchPending = true;
  ImGui::SameLine();
  if (ImGui::Button("Search", ImVec2(-1, 0)))
    mSearchPending = true;

  const SearchState state = mTone3000.GetSearchState();
  const std::string status = mTone3000.GetStatusMessage();

  // One status line, always drawn, so the list below never shifts under the pointer when a search
  // starts or a message appears.
  std::string line;
  ImVec4 lineColor = theme::TextDim();
  if (searching)
  {
    line = "Searching...";
  }
  else if (noFolder)
  {
    line = "Choose a capture folder in Settings - downloads are saved there.";
    lineColor = theme::Warning();
  }
  else if (!status.empty() && !state.error.empty() && status == state.error)
  {
    line = status;
    lineColor = theme::Danger();
  }
  else if (!state.results.empty())
  {
    line = std::to_string(state.total) + (state.total == 1 ? " result" : " results");
  }
  else if (!status.empty())
  {
    line = status;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, lineColor);
  ImGui::TextUnformatted(line.empty() ? " " : line.c_str());
  ImGui::PopStyleColor();

  if (state.results.empty())
    return;

  // What is already downloaded, by the folder name each tone lands in. Offering to fetch something
  // twice is the kind of thing that makes a library view feel careless.
  std::unordered_set<std::string> inLibrary;
  for (const auto& entry : mCaptureLibrary.GetEntries())
    if (!entry.groupName.empty())
      inLibrary.insert(entry.groupName);

  const float footer = ImGui::GetFrameHeightWithSpacing();
  // Only the columns the API can actually order by are sortable. Sorting a page of twenty out of
  // five hundred results locally would look like sorting and not be it, so clicking a header here
  // re-runs the search with that order instead - which is why it also moves the sidebar's Sort
  // control, rather than the two quietly disagreeing.
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX
                                     | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;

  if (ImGui::BeginTable("##tonetable", 4, kFlags, ImVec2(0, -footer)))
  {
    ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
    ImGui::TableSetupColumn("Models", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 58.0f);
    // Descending only: "most downloaded" is the order the endpoint offers, and there is no
    // fewest-downloads sort to pretend to have.
    ImGui::TableSetupColumn("Downloads",
                            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending
                              | ImGuiTableColumnFlags_NoSortAscending,
                            78.0f, kCloudColumnDownloads);
    ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 96.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
    {
      const bool wantsDownloads = specs->SpecsCount > 0 && specs->Specs[0].ColumnUserID == kCloudColumnDownloads;
      if (wantsDownloads && mFilters.sort != SortOrder::DownloadsAllTime)
      {
        mFilters.sort = SortOrder::DownloadsAllTime;
        mSearchPending = true;
      }
    }

    for (const auto& tone : state.results)
    {
      ImGui::TableNextRow();
      ImGui::PushID(static_cast<int>(tone.id));

      // Two lines per row, always: title, then who and what it is. Wrapped headings gave every
      // row a different height, which is what made the old list impossible to scan.
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(tone.title.c_str(), mSelectedToneId == tone.id,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
        mSelectedToneId = tone.id;

      if (ImGui::BeginPopupContextItem("##tonemenu"))
      {
        mSelectedToneId = tone.id;
        if (ImGui::MenuItem("Download", nullptr, false, !noFolder && !mTone3000.IsDownloadingTone(tone.id)))
          mTone3000.DownloadToneAsync(tone, mCaptureLibrary.GetFolder());
        ImGui::EndPopup();
      }

      std::string byLine = tone.gear;
      if (!tone.userName.empty())
        byLine += (byLine.empty() ? "" : "   ") + std::string("by ") + tone.userName;
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::TextUnformatted(byLine.empty() ? "-" : byLine.c_str());
      ImGui::PopStyleColor();

      if (!tone.tags.empty())
      {
        ImGui::SameLine(0, 10);
        float budget = ImGui::GetContentRegionAvail().x;
        for (size_t t = 0; t < tone.tags.size(); t++)
        {
          const float width = theme::ChipWidth(tone.tags[t].c_str()) + (t > 0 ? 4.0f : 0.0f);
          if (width > budget)
            break;
          if (t > 0)
            ImGui::SameLine(0, 4);
          theme::Chip(tone.tags[t].c_str(), theme::TextDim());
          budget -= width;
        }
      }

      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%d", tone.modelsCount);

      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
      ImGui::Text("%d", tone.downloadsCount);
      ImGui::PopStyleColor();

      ImGui::TableSetColumnIndex(3);
      if (mTone3000.IsDownloadingTone(tone.id))
      {
        // The spinner takes the button's place rather than sitting beside it, so the row does not
        // change width the moment you press it.
        theme::Spinner(ImGui::GetFrameHeight());
      }
      else if (inLibrary.count(ToneFolderName(tone.title)) > 0)
      {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Success());
        ImGui::TextUnformatted("In library");
        ImGui::PopStyleColor();
      }
      else
      {
        ImGui::BeginDisabled(noFolder);
        if (ImGui::Button("Download", ImVec2(-1, 0)))
          mTone3000.DownloadToneAsync(tone, mCaptureLibrary.GetFolder());
        ImGui::EndDisabled();
      }

      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Pager, right-aligned: it belongs to the list rather than to the page as a whole.
  const float pagerWidth = 210.0f;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - pagerWidth));

  ImGui::BeginDisabled(searching || state.page <= 1);
  if (ImGui::Button("Prev"))
    mTone3000.SearchAsync(mFilters, state.page - 1);
  ImGui::EndDisabled();

  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::Text("%d / %d", state.page, state.totalPages);
  ImGui::PopStyleColor();

  ImGui::SameLine();
  ImGui::BeginDisabled(searching || state.page >= state.totalPages);
  if (ImGui::Button("Next"))
    mTone3000.SearchAsync(mFilters, state.page + 1);
  ImGui::EndDisabled();
}

void MainWindow::DrawToneDetails()
{
  const SearchState state = mTone3000.GetSearchState();

  const ToneSummary* tone = nullptr;
  for (const auto& candidate : state.results)
    if (candidate.id == mSelectedToneId)
      tone = &candidate;

  if (tone == nullptr)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped("Select a result to see its details.");
    ImGui::PopStyleColor();
    return;
  }

  theme::PushHeading(1.05f);
  ImGui::TextWrapped("%s", tone->title.c_str());
  theme::PopFont();

  const bool isIr = (tone->format == "ir");
  theme::Chip(isIr ? "IR" : "NAM", BlockColor(isIr ? BlockType::Ir : BlockType::Nam));
  ImGui::Separator();

  const auto field = [](const char* label, const std::string& value)
  {
    if (value.empty())
      return;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(110.0f);
    ImGui::TextWrapped("%s", value.c_str());
  };

  field("Creator", tone->userName);
  field("Gear", tone->gear);
  field("License", tone->license);
  field("Models", std::to_string(tone->modelsCount));
  field("Downloads", std::to_string(tone->downloadsCount));
  if (tone->a2ModelsCount > 0 || tone->a1ModelsCount > 0)
    field("Architecture", tone->a2ModelsCount > 0 ? "A2" : "A1");

  if (!tone->makes.empty())
  {
    std::string makes;
    for (const auto& make : tone->makes)
      makes += (makes.empty() ? "" : ", ") + make;
    field("Makes", makes);
  }

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
  ImGui::TextUnformatted("TAGS");
  ImGui::PopStyleColor();

  if (tone->tags.empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextUnformatted("none");
    ImGui::PopStyleColor();
  }
  else
  {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    for (size_t t = 0; t < tone->tags.size(); t++)
    {
      const float width = theme::ChipWidth(tone->tags[t].c_str());
      if (t > 0)
      {
        ImGui::SameLine(0, 4);
        if (ImGui::GetCursorPosX() + width > right)
          ImGui::NewLine();
      }
      theme::Chip(tone->tags[t].c_str(), theme::TextDim());
    }
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  const bool have = [&]
  {
    const std::string folder = ToneFolderName(tone->title);
    for (const auto& entry : mCaptureLibrary.GetEntries())
      if (entry.groupName == folder)
        return true;
    return false;
  }();

  if (mTone3000.IsDownloadingTone(tone->id))
  {
    theme::Spinner(ImGui::GetFrameHeight());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted("Downloading");
    ImGui::PopStyleColor();
  }
  else if (have)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Success());
    ImGui::TextUnformatted("Already in your library");
    ImGui::PopStyleColor();
    // Still offered: a pack can gain models after you first downloaded it.
    if (ImGui::Button("Download again", ImVec2(-1, 0)))
      mTone3000.DownloadToneAsync(*tone, mCaptureLibrary.GetFolder());
  }
  else
  {
    ImGui::BeginDisabled(mCaptureLibrary.GetFolder().empty());
    if (ImGui::Button("Download", ImVec2(-1, 0)))
      mTone3000.DownloadToneAsync(*tone, mCaptureLibrary.GetFolder());
    ImGui::EndDisabled();
  }
}

void MainWindow::DrawCloudFilters()
{
  const float rowWidth = ImGui::GetContentRegionAvail().x;
  theme::PushHeading(1.1f);
  ImGui::TextUnformatted("Filters");
  theme::PopFont();
  ImGui::SameLine(rowWidth - ImGui::CalcTextSize("Clear").x - ImGui::GetStyle().FramePadding.x * 2.0f);
  if (ImGui::SmallButton("Clear"))
  {
    const SortOrder keptSort = mFilters.sort;
    mFilters = SearchFilters();
    mFilters.sort = keptSort;
    mVerifiedCreatorsOnly = false;
    mSearchPending = true;
  }
  ImGui::Separator();

  if (ImGui::CollapsingHeader("Format", ImGuiTreeNodeFlags_DefaultOpen))
  {
    // The app plays both now: .nam captures in PRE/AMP and IR wavs in the IR slot.
    int format = (mFilters.format == SearchFormat::Ir) ? 1 : 0;
    bool changed = false;
    changed |= ImGui::RadioButton("NAM captures", &format, 0);
    changed |= ImGui::RadioButton("Impulse responses", &format, 1);
    if (changed)
    {
      mFilters.format = (format == 1) ? SearchFormat::Ir : SearchFormat::Nam;
      mSearchPending = true;
    }
  }

  if (ImGui::CollapsingHeader("Sort", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static const char* kSortLabels[] = {"Best match", "Newest", "Oldest", "Trending", "Most downloaded"};
    int sortIndex = static_cast<int>(mFilters.sort);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##sort", &sortIndex, kSortLabels, IM_ARRAYSIZE(kSortLabels)))
    {
      mFilters.sort = static_cast<SortOrder>(sortIndex);
      mSearchPending = true;
    }
  }

  if (ImGui::CollapsingHeader("Gear"))
    for (const auto& option : kGearOptions)
      if (FilterCheckbox(option.label, mFilters.gears, option.value))
        mSearchPending = true;

  if (ImGui::CollapsingHeader("Technical"))
  {
    int architecture = static_cast<int>(mFilters.architecture);
    bool changed = false;
    changed |= ImGui::RadioButton("Any architecture", &architecture, 0);
    changed |= ImGui::RadioButton("A2 - default", &architecture, 1);
    changed |= ImGui::RadioButton("A1 - legacy", &architecture, 2);
    changed |= ImGui::RadioButton("Custom", &architecture, 3);
    if (changed)
    {
      mFilters.architecture = static_cast<Architecture>(architecture);
      mSearchPending = true;
    }
    if (theme::Check("Calibrated only", &mFilters.calibratedOnly))
      mSearchPending = true;
  }

  if (ImGui::CollapsingHeader("Size"))
    for (const auto& option : kSizeOptions)
      if (FilterCheckbox(option.label, mFilters.sizes, option.value))
        mSearchPending = true;

  if (ImGui::CollapsingHeader("Tags"))
  {
    if (DrawSelectedFilters(mFilters.tags))
      mSearchPending = true;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##tagsearch", "Search tags", mTagFilterText, sizeof(mTagFilterText)))
      mTone3000.FetchTagsAsync(mTagFilterText);
    if (DrawTaxonomyList("##taglist", mTone3000.GetTags(), mFilters.tags, false))
      mSearchPending = true;
  }

  if (ImGui::CollapsingHeader("Makes and models"))
  {
    if (DrawSelectedFilters(mFilters.makes))
      mSearchPending = true;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##makesearch", "Search makes", mMakeFilterText, sizeof(mMakeFilterText)))
      mTone3000.FetchMakesAsync(mMakeFilterText);
    if (DrawTaxonomyList("##makelist", mTone3000.GetMakes(), mFilters.makes, false))
      mSearchPending = true;
  }

  if (ImGui::CollapsingHeader("Creators"))
  {
    if (DrawSelectedFilters(mFilters.creators))
      mSearchPending = true;
    // The users endpoint has no verified flag, so this filters the fetched page client-side.
    theme::Check("Verified only", &mVerifiedCreatorsOnly);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##creatorsearch", "Search creators", mCreatorFilterText, sizeof(mCreatorFilterText)))
      mTone3000.FetchCreatorsAsync(mCreatorFilterText);
    if (DrawTaxonomyList("##creatorlist", mTone3000.GetCreators(), mFilters.creators, mVerifiedCreatorsOnly))
      mSearchPending = true;
  }
}

} // namespace nam_ui





