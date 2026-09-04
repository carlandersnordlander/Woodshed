#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Config.h"
#include "NoteAnalysis.h"
#include "ParametricEq.h"

namespace nam_ui
{

/// \brief Everything the Play view holds, saved as a folder you can move, copy or back up.
///
/// A project is a folder, not a file: the audio it refers to is copied in beside the settings, so
/// opening it a year later works whether or not the stems are still where Demucs left them. The
/// `.shed` inside is what you open, and it names its audio by relative path.
struct ProjectFile
{
  /// The extension of the file you open. The folder around it is the project.
  static constexpr const char* kExtension = ".shed";
  /// What projects were called before the app was named. Still opened, still listed, and never
  /// written: a file somebody made last month is not made unopenable by a change of name.
  static constexpr const char* kLegacyExtension = ".namproj";

  struct TrackEntry
  {
    /// Relative to the project folder, always - an absolute path would break the moment the
    /// folder moved.
    std::string relativePath;
    std::string name;
    float gainDb = 0.0f;
    bool muted = false;
    bool soloed = false;
    bool eqEnabled = false;
    ParametricEqSettings eq;
    /// What the note detector found, if it has been run. Analysis costs seconds and never changes
    /// for a given file, so a project remembers it rather than asking for it again.
    NoteTrack notes;
  };

  struct LoopEntry
  {
    std::string name;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
  };

  std::string name;
  /// The folder the project lives in. Filled in by Save and by Load, and what track paths are
  /// relative to.
  std::filesystem::path folder;
  std::vector<TrackEntry> tracks;
  std::vector<LoopEntry> loops;
  int activeLoop = -1;

  // --- the transport and the mix ---
  float gainDb = 0.0f;
  float speed = 1.0f;
  float semitones = 0.0f;
  double positionSeconds = 0.0;

  // --- the grid ---
  bool tempoValid = false;
  float bpm = 120.0f;
  double firstBeatSeconds = 0.0;
  float confidence = 0.0f;
  /// The tracked grid: where every beat actually falls. Worth keeping - it is the expensive half
  /// of the analysis, and a song's beats do not change between sessions.
  std::vector<double> beats;
  int downbeatOffset = 0;
  float lowestBpm = 0.0f;
  float highestBpm = 0.0f;
  int beatsPerBar = 4;
  bool showGrid = true;
  bool snapToGrid = true;

  // --- what the timeline is looking at ---
  double viewStart = 0.0;
  double viewDuration = 0.0;
  bool followPlayhead = true;

  /// The click as the transport had it, including whether it was armed.
  AppConfig::MetronomePreset metronome;
  bool clickArmed = false;

  /// Where projects are kept unless you put one somewhere else: %APPDATA%/Woodshed/projects.
  static std::filesystem::path DefaultFolder();
  /// The project file inside a project folder, whichever of the two extensions it carries, or
  /// empty when the folder holds none.
  static std::filesystem::path FileIn(const std::filesystem::path& folder);
  /// The folder a project of this name gets, with anything a path cannot carry replaced.
  static std::filesystem::path FolderFor(const std::string& name);
  /// The file Save() writes inside `folder` for a project of this name.
  static std::filesystem::path FileFor(const std::filesystem::path& folder, const std::string& name);

  /// \brief Writes the project into `folder`, copying every source file into `folder/audio`.
  ///
  /// \param sources one absolute path per track, in the same order as `tracks`. On return each
  ///        track's relativePath points at the copy.
  /// \param error something worth showing a person when this returns false
  bool Save(const std::filesystem::path& folder, const std::vector<std::filesystem::path>& sources,
            std::string& error);

  /// Reads a `.namproj`. `folder` comes back as the file's own folder, which is what every track's
  /// relativePath is relative to.
  static bool Load(const std::filesystem::path& file, ProjectFile& out, std::string& error);

  /// Every project in the default folder, by name.
  static std::vector<std::string> List();
};

} // namespace nam_ui
