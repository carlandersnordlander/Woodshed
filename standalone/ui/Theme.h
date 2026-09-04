#pragma once

#include "imgui.h"

namespace nam_ui::theme
{

/// Applies the app's colours, rounding and spacing to the current ImGui style.
/// Call once at startup, before ImGuiStyle::ScaleAllSizes().
void Apply();

/// Loads the UI fonts from the system font directory. Falls back to Dear ImGui's built-in font
/// if the files aren't there, so this never prevents the app from starting.
void LoadFonts();

/// Slightly larger semibold face for titles and section headings. Never null - falls back to the
/// body font, which in turn falls back to the built-in one.
ImFont* Heading();

/// The default UI face.
ImFont* Body();

/// Pushes the heading face at `sizeScale` times the base size. Since 1.92 a font can be rendered
/// at any size, so this wraps the two-argument PushFont and keeps size maths out of the UI code.
/// Must be paired with PopFont().
void PushHeading(float sizeScale = 1.0f);

/// Pops whatever PushHeading() pushed.
void PopFont();

// --- palette ------------------------------------------------------------------------------
//
// Warm amber against cool near-black greys: the tube-glow look, and the two temperatures keep the
// accent legible without it having to shout.
//
// The greys are a ladder rather than a set. Depth is what tells a panel from the space around it
// and a button from a field, and doing that with light is quieter and reads faster than drawing a
// line around everything. Going up the ladder means nearer the front:
//
//     Sunken   a hole in the panel: text fields, wells, tracks
//     Base     the app behind everything
//     Surface  a panel sitting on the base
//     Raised   a button or a popup sitting on a panel
//     Hover    the same, under the pointer

ImVec4 Accent();
ImVec4 AccentDim();
/// The mark's own cream. Warmer than the white the controls use, and only ever the logo's.
ImVec4 Wood();
ImVec4 Success();
ImVec4 Danger();
ImVec4 Warning();

ImVec4 Base();
ImVec4 Surface();
/// The app's own chrome - the navigation rail and the input/output strip. A touch off black, so
/// the frame around the work is distinguishable from the work without being a different colour.
ImVec4 Rail();
ImVec4 Raised();
ImVec4 Hover();
ImVec4 Sunken();
ImVec4 Line();

/// Body text.
ImVec4 Text();
/// Labels and secondary information: present, but not competing with the value beside it.
ImVec4 TextDim();
/// Barely there. Units, hints, disabled things.
ImVec4 TextFaint();

/// The colour of a control itself - pointers, handles, glyphs. White against the black panel.
ImVec4 Control();

/// A section heading: heading font, faint, with space above it and none below - it belongs to what
/// comes after.
void SectionLabel(const char* text);

/// The name of the control beside it. Dim, so the value next to it is what the eye lands on.
///
/// Worth its own function because the alternative is three lines of colour push and pop at every
/// one of the hundred or so places a control has a name, and three lines is enough friction that
/// some of them end up not matching the rest.
void Label(const char* text);

/// The same, followed by SameLine and a fixed column width - for rows of labelled controls that
/// should line up down the left rather than each starting wherever its own label ended.
void LabelFor(const char* text, float width = 0.0f);

/// A number worth reading from across the room: heading face, full-strength text.
void Value(const char* text, float sizeScale = 1.15f);

/// A rule with air either side. Quieter than Separator() on its own, which sits tight against
/// whatever is above it.
void Divider();

/// Explanatory text under a control. Faint and wrapped - there to be found when wanted rather than
/// read every time.
void Hint(const char* text);

// --- layout ---------------------------------------------------------------------------------
//
// What makes a screen look designed rather than assembled is not the widgets, it is the space
// between them: things that belong together sitting together, and a visible gap where the subject
// changes. ImGui will happily put fifteen controls in one unbroken row, and left to itself that is
// what a screen becomes. These are the tools for not doing that.

/// A padded surface with an optional heading, for one group of related controls.
///
/// \param size as BeginChild - zero in either axis fills the available space
/// \return false when it is clipped, exactly like BeginChild; call EndCard() regardless
bool BeginCard(const char* id, const char* heading = nullptr, ImVec2 size = ImVec2(0, 0));
void EndCard();

/// A vertical rule between two clusters of controls in a toolbar, with the right air either side.
/// Leaves the cursor on the same line, so it goes between two SameLine runs.
///
/// A row of buttons all spaced alike reads as one long list of unrelated things; the same row
/// broken into three groups reads as three decisions.
void ToolbarSeparator();

/// The space between two clusters that do not warrant a rule between them.
void ToolbarGap();

/// A view's title, for the top of a screen.
void ViewTitle(const char* text);

// --- icons ----------------------------------------------------------------------------------
//
// Drawn rather than typed. The UI font is loaded with the Latin ranges only, so a play triangle or
// a loop arrow would come out as a box - and the ASCII stand-ins that were there instead ("|<",
// "<<") read as punctuation rather than as controls. These are paths, so they stay sharp at any
// size and take the colour of whatever state they are in.
enum class Icon
{
  SkipStart,
  Rewind,
  Play,
  Pause,
  Stop,
  Forward,
  Previous,
  Next,
  Loop,
  Metronome,
  Plus,
  Minus,
  Grid,
  Ruler,
  Eye,
  Sliders,
  Volume,
  Note
};

/// Draws one icon centred in a box, without taking any layout space.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 centre, float size, ImU32 colour);

/// \brief The mark: three tree rings, cut through by one notch.
///
/// Drawn from its geometry rather than loaded from the file, because the app has no SVG renderer
/// and the shape is three arcs and a dot - and because drawn, it is sharp at any size and on any
/// screen, and takes its colours from the theme instead of carrying its own.
///
/// Below about twenty-six pixels it draws the single-ring version instead, which is the shape the
/// drawings give for that size: three rings that small are three grey lines.
///
/// \param size the mark's full diameter
void Logo(ImDrawList* draw, ImVec2 centre, float size, ImU32 accent, ImU32 wood);

/// \brief The mark with the name beside it, laid out as the horizontal lockup.
///
/// \param left where the mark's left edge goes
/// \param centreY the line the mark and the wordmark are both centred on
/// \param height the mark's diameter; everything else is a proportion of it
/// \return the width the whole lockup took
float LogoLockup(ImDrawList* draw, float left, float centreY, float height, ImU32 accent, ImU32 wood);

/// How an icon button is filled.
enum class IconStyle
{
  Bare,    ///< no background until hovered - for a row of secondary actions
  Raised,  ///< a circle that is always there
  Primary  ///< filled in the accent colour: the one thing on the bar you press most
};

/// A round icon button.
/// \param size the diameter; the glyph is drawn to suit
/// \return true when clicked
bool IconButton(const char* id, Icon icon, float size, IconStyle style = IconStyle::Bare, bool active = false);

/// \brief A slim slider: a thin track, the part up to the value filled, and a round handle.
///
/// The same shape as the transport's scrub bar, deliberately. A channel's level and a song's
/// position are both "one value along a line", and drawing them in two different ways said they
/// were two different kinds of thing.
///
/// The handle is grey until the pointer is on it, so the accent is spent on the moment the control
/// is being used rather than on it sitting there. Double-click returns it to `defaultValue`.
///
/// \param format printf format for the tooltip shown while it is being moved
/// \return true while the value is being changed
bool SlimSlider(const char* id, float* value, float minValue, float maxValue, float defaultValue, float width,
                const char* format = "%.1f");

/// The same control stood on end, for a level that pops up over the thing it belongs to.
bool SlimSliderVertical(const char* id, float* value, float minValue, float maxValue, float defaultValue, float height,
                        const char* format = "%.1f");

/// \brief A small square toggle with a letter in it - the M and S of a mixer strip.
///
/// A checkbox with a label beside it is two objects that happen to sit together, and eight tracks
/// of them is sixteen little boxes and sixteen loose letters. One square with the letter inside is
/// one object, and it is the shape every mixer ever built uses for this.
///
/// \param lit the fill when it is on. Mute and solo take different ones so a glance down the
///        stack says which is which without reading any of them
/// \return true if it was toggled
bool LetterToggle(const char* id, const char* letter, bool* on, float size, ImVec4 lit);

/// \brief A transport key: one flat rounded rectangle with a glyph in it.
///
/// Deliberately without material - no bevel, no gradient, no glow. Those say "this is pretending
/// to be a physical button", and at this size the pretence is the only thing that reads. What
/// separates these from each other is size, shape and colour, which is enough and stays clean.
///
/// Square-ish rather than round: a circle with a triangle in it is the shape every media player
/// uses, and being mistaken for one is the thing to avoid.
///
/// \param lit    the accent fill - for the one key that is doing something right now
/// \param active a quieter accent, for a mode that is armed rather than running
/// \return true when clicked
bool TransportKey(const char* id, Icon icon, ImVec2 size, bool lit = false, bool active = false);

/// \brief A name with a caret after it: press it and a menu of everything to do with that name
/// opens under it.
///
/// The shape the project name uses. Where a thing has a name and a handful of actions that all
/// belong to it - load another, save this one, throw it away - a row of buttons gives each action
/// the same weight as the name, and the name is the part you read. This puts the name first and
/// the actions one press away.
///
/// The caret is drawn rather than typed: the UI font carries the Latin ranges only, so a real
/// caret glyph comes out as a box, and the letter "v" beside a name reads as part of the name.
///
/// \param label what to show; pass "Name###id" where the text changes but the item should not
/// \param sizeScale heading size for the name, 1.0 being the body face
/// \param minWidth pads the button out, for a menu that should not resize as names change
/// \return true when pressed - call ImGui::OpenPopup() and draw the menu yourself
bool MenuButton(const char* label, float sizeScale = 1.0f, float minWidth = 0.0f);

/// Horizontal level meter with a colour that shifts toward red as it approaches clipping.
/// \param levelDb peak level in dB; the meter floor is -60 dB
void LevelMeter(const char* id, float levelDb, float width);

/// Peak level, in dBFS, of the band we steer the player's input level into. NAM captures are
/// level-sensitive, so landing in here is what makes a capture sound the way it was trained.
constexpr float kInputTargetLowDb = -18.0f;
constexpr float kInputTargetHighDb = -6.0f;

/// Tall meter with an optional target band drawn behind the level - the gain-staging display.
/// \param height in pixels; use something generous, this is meant to be readable across a room
void BigMeter(float levelDb, float width, float height, bool showTargetZone);

/// Mixer-style vertical meter, filling from the bottom up.
void VerticalMeter(float levelDb, float width, float height, bool showTargetZone);

/// A rotary control, the way an amp or pedal has them rather than a horizontal slider.
///
/// Drag up and down to turn it, hold Shift for fine adjustment, double-click to return it to
/// `defaultValue`. When the range straddles zero the arc fills outward from the centre, which is
/// how a cut/boost control should read.
///
/// Optionally it has detents: values it settles onto as you pass them, and that take a deliberate
/// extra pull to leave. Useful where particular settings mean something - fully wet, unity gain -
/// and landing exactly on them by hand is otherwise fiddly.
///
/// \param format printf format for the value shown under the knob, e.g. "%.1f dB"
/// \param detents values to settle on, or nullptr
/// \return true while the value is being changed
/// \param showName false draws only the value and puts the name in a tooltip, for a knob in a
///        column too narrow or too full to spend a line on a word you already know
/// \param arcColour overrides the accent on the value arc - hand it a level and the knob is its
///        own meter
bool Knob(const char* label, float* value, float minValue, float maxValue, float defaultValue, const char* format,
          float diameter = 62.0f, const float* detents = nullptr, int detentCount = 0, bool showName = true,
          const ImVec4* arcColour = nullptr);

/// \brief A colour for a level: green with room to spare, amber where it should sit, red as it
/// runs out.
///
/// Meant for a knob's arc, so a control that sets a level also shows what that level is doing and
/// the two never have to be read in two places.
ImVec4 LevelColour(float levelDb);

/// True if the knob or fader drawn immediately before this call was just released - the moment
/// worth persisting to disk.
bool KnobReleased();

/// The standard power symbol - a broken ring with a stroke through the gap - used everywhere
/// something is switched in or out. Lit in the accent colour when on, dim when off.
/// \return true if the state was toggled
bool PowerButton(const char* id, bool* on, float size);

/// A five-pointed star: filled in the accent colour when set, outlined when not.
/// \return true if it was clicked
bool StarButton(const char* id, bool filled, float size);

/// \brief A checkbox: an outlined square with a tick in it, and its label beside it.
///
/// Drawn here rather than left to ImGui so every one in the app is the same object. ImGui's own
/// fills the box with the frame colour and puts the accent inside it, which next to a row of flat
/// white controls reads as the one thing borrowed from somewhere else.
/// \return true if it was toggled
bool Check(const char* label, bool* value);

/// Two crossed strokes - the remove glyph. Turns red under the pointer, because what it does
/// cannot be undone by clicking it again.
/// \return true if it was clicked
bool CloseButton(const char* id, float size);

/// A rotating arc, for work whose length is not known ahead of time. Takes the same layout space
/// as a square of `size`, so it can stand in for the button it replaces without the row reflowing.
void Spinner(float size);

/// The same, but you can press it - for work that has been sent to the background and needs a way
/// back to whatever was reporting on it.
/// \return true when clicked
bool SpinnerButton(const char* id, float size);

/// A small tinted label: a type marker, a tag, a state. Reads as one object at a glance, which a
/// run of words joined by dashes never does. Takes layout space like any other item.
void Chip(const char* text, ImVec4 color);

/// Width Chip() would occupy, for laying out a row of them against a budget.
float ChipWidth(const char* text);

/// A vertical fader: a track with a handle, the way a mixer strip has them.
/// \return true while the value is being changed
bool Fader(const char* label, float* value, float minValue, float maxValue, float defaultValue, const char* format,
           float width, float height);

/// One strobe band: a row of blocks scrolled horizontally by `phase`.
///
/// The illusion is the whole point - the pattern only appears to stand still when the phase stops
/// advancing, which happens exactly when the played pitch matches the reference. Drift right means
/// sharp, left means flat, and the speed is the size of the error.
///
/// \param phase band phase in radians, from Tuner::GetBandPhase()
/// \param strength 0..1; fades the band out when there is nothing at that frequency
/// \param inTune tints the band once the note is close enough to call good
void StrobeStrip(float phase, float strength, float width, float height, int blocks, bool inTune);

/// The other way to read a tuner: a needle over a scale of cents. Coarser than the strobe - it
/// cannot resolve a fraction of a cent - but it says which way to turn the peg at a glance, which
/// is what you want until the last few cents.
///
/// \param cents signed error; the scale runs to +/-50, where the next semitone begins
void NeedleGauge(float cents, bool hasSignal, bool inTune, float width, float height);

/// True when the level sits inside the input target band.
bool InInputTargetZone(float levelDb);

} // namespace nam_ui::theme
