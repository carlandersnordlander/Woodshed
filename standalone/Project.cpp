#include "Project.h"

#include <algorithm>
#include <fstream>

#include "ConfigJson.h"
#include "json.hpp"

namespace nam_ui
{

namespace
{

/// A project name is typed by hand and becomes a folder name, so anything a path cannot carry is
/// replaced rather than refused.
std::string SafeName(const std::string& name)
{
  std::string safe;
  safe.reserve(name.size());
  for (const unsigned char c : name)
  {
    if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?'
        || c == '*')
      safe.push_back('_');
    else
      safe.push_back(static_cast<char>(c));
  }
  while (!safe.empty() && (safe.back() == '.' || safe.back() == ' '))
    safe.pop_back();
  if (safe.empty())
    safe = "project";
  return safe;
}

nlohmann::json WriteEq(const ParametricEqSettings& eq)
{
  nlohmann::json bands = nlohmann::json::array();
  for (const auto& band : eq.bands)
    bands.push_back({{"enabled", band.enabled}, {"hz", band.hz}, {"gainDb", band.gainDb}, {"q", band.q}});
  return bands;
}

ParametricEqSettings ReadEq(const nlohmann::json& j)
{
  ParametricEqSettings eq;
  if (!j.is_array())
    return eq;

  size_t index = 0;
  for (const auto& stored : j)
  {
    if (index >= kEqBandCount)
      break;
    if (!stored.is_object())
      continue;
    EqBand& band = eq.bands[index++];
    band.enabled = stored.value("enabled", band.enabled);
    band.hz = stored.value("hz", band.hz);
    band.gainDb = stored.value("gainDb", band.gainDb);
    band.q = stored.value("q", band.q);
  }
  return eq;
}

/// Short keys: a bass line is hundreds of notes, and this is the one part of a project that grows
/// with the music rather than with the settings.
nlohmann::json WriteNotes(const NoteTrack& notes)
{
  nlohmann::json out = nlohmann::json::object();
  out["visible"] = notes.visible;
  out["polyphonic"] = notes.polyphonic;
  out["playNotes"] = notes.playNotes;
  out["playChords"] = notes.playChords;
  out["instrument"] = notes.instrument;
  out["playGainDb"] = notes.playGainDb;
  out["playOctave"] = notes.playOctave;
  out["low"] = notes.lowestMidi;
  out["high"] = notes.highestMidi;
  out["clean"] = notes.cleanupStrength;
  out["voices"] = notes.maxVoices;

  // What the detector found, not what survived the cleanup. Larger, and the reason the cleanup can
  // still be moved after the project has been closed and opened again - which is the only way to
  // find the right amount for a recording.
  nlohmann::json array = nlohmann::json::array();
  for (const auto& note : notes.rawNotes)
    array.push_back({{"t", note.startSeconds},
                     {"e", note.endSeconds},
                     {"m", note.midi},
                     {"c", note.centsOffset},
                     {"q", note.confidence}});
  out["raw"] = array;

  nlohmann::json chordArray = nlohmann::json::array();
  for (const auto& chord : notes.chords)
    chordArray.push_back({{"t", chord.startSeconds},
                          {"e", chord.endSeconds},
                          {"r", chord.root},
                          {"k", static_cast<int>(chord.quality)},
                          {"q", chord.confidence}});
  out["chords"] = chordArray;
  return out;
}

NoteTrack ReadNotes(const nlohmann::json& j)
{
  NoteTrack out;
  if (!j.is_object())
    return out;

  out.visible = j.value("visible", out.visible);
  out.polyphonic = j.value("polyphonic", out.polyphonic);
  out.playNotes = j.value("playNotes", out.playNotes);
  out.playChords = j.value("playChords", out.playChords);
  out.instrument = j.value("instrument", out.instrument);
  out.playGainDb = j.value("playGainDb", out.playGainDb);
  out.playOctave = std::clamp(j.value("playOctave", out.playOctave), -3, 3);
  out.lowestMidi = j.value("low", out.lowestMidi);
  out.highestMidi = j.value("high", out.highestMidi);

  out.cleanupStrength = std::clamp(j.value("clean", out.cleanupStrength), 0.0f, 1.0f);
  out.maxVoices = std::clamp(j.value("voices", out.maxVoices), 0, 16);

  // "raw" is everything the detector found; "notes" is what a project written before the cleanup
  // existed had, which is the same thing under an older name. Read as raw either way - but a file
  // from before shows exactly what it showed then, rather than being quietly cleaned on opening.
  const char* const key = (j.contains("raw") && j["raw"].is_array()) ? "raw" : "notes";
  if (key[0] == 'n')
    out.cleanupStrength = 0.0f;

  if (j.contains(key) && j[key].is_array())
  {
    for (const auto& stored : j[key])
    {
      if (!stored.is_object())
        continue;
      DetectedNote note;
      note.startSeconds = stored.value("t", note.startSeconds);
      note.endSeconds = stored.value("e", note.endSeconds);
      note.midi = stored.value("m", note.midi);
      note.centsOffset = stored.value("c", note.centsOffset);
      note.confidence = stored.value("q", note.confidence);
      out.rawNotes.push_back(note);
    }
  }

  if (j.contains("chords") && j["chords"].is_array())
  {
    for (const auto& stored : j["chords"])
    {
      if (!stored.is_object())
        continue;
      DetectedChord chord;
      chord.startSeconds = stored.value("t", chord.startSeconds);
      chord.endSeconds = stored.value("e", chord.endSeconds);
      chord.root = stored.value("r", chord.root);
      chord.quality = static_cast<ChordQuality>(stored.value("k", 0));
      chord.confidence = stored.value("q", chord.confidence);
      out.chords.push_back(chord);
    }
  }

  ApplyNoteCleanup(out);
  // The range was stored, but the cleanup may have taken the highest and lowest notes with it, and
  // a lane laid out for notes that are no longer drawn has empty space at both ends.
  if (out.rawNotes.empty())
  {
    out.lowestMidi = j.value("low", out.lowestMidi);
    out.highestMidi = j.value("high", out.highestMidi);
  }

  out.valid = !out.rawNotes.empty() || !out.chords.empty();
  return out;
}

} // namespace

std::filesystem::path ProjectFile::DefaultFolder()
{
  return AppConfig::GetConfigFolder() / "projects";
}

std::filesystem::path ProjectFile::FileIn(const std::filesystem::path& folder)
{
  // The current extension wins where a folder somehow has both, so a project saved again is the
  // one that opens rather than the copy it was saved from.
  std::error_code ec;
  std::filesystem::path legacy;

  for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
  {
    if (ec || !entry.is_regular_file(ec))
      continue;
    if (entry.path().extension() == kExtension)
      return entry.path();
    if (entry.path().extension() == kLegacyExtension)
      legacy = entry.path();
  }

  return legacy;
}

std::filesystem::path ProjectFile::FolderFor(const std::string& name)
{
  return DefaultFolder() / SafeName(name);
}

std::filesystem::path ProjectFile::FileFor(const std::filesystem::path& folder, const std::string& name)
{
  return folder / (SafeName(name) + kExtension);
}

bool ProjectFile::Save(const std::filesystem::path& target, const std::vector<std::filesystem::path>& sources,
                       std::string& error)
{
  std::error_code ec;
  const std::filesystem::path audioFolder = target / "audio";
  std::filesystem::create_directories(audioFolder, ec);
  if (ec)
  {
    error = "Could not create " + audioFolder.string();
    return false;
  }

  folder = target;

  // --- the audio, copied in ---
  //
  // A project that pointed at wherever the stems happened to be would break the first time that
  // folder was tidied up. Copying is a few hundred megabytes for a full separation, which is the
  // price of the whole thing still working next year.
  for (size_t i = 0; i < tracks.size(); i++)
  {
    if (i >= sources.size() || sources[i].empty())
      continue;

    const std::filesystem::path source = sources[i];

    // Already inside this project - it was opened from here, or saved here before.
    const std::filesystem::path existing = audioFolder / source.filename();
    if (std::filesystem::exists(existing, ec) && std::filesystem::equivalent(source, existing, ec))
    {
      tracks[i].relativePath = "audio/" + source.filename().string();
      continue;
    }

    // Two stems can arrive called bass.wav from different songs, so a name already taken gets a
    // number rather than quietly overwriting the file that is there.
    std::filesystem::path destination = audioFolder / source.filename();
    for (int attempt = 2; std::filesystem::exists(destination, ec) && attempt < 100; attempt++)
    {
      const std::string stem = source.stem().string() + " (" + std::to_string(attempt) + ")";
      destination = audioFolder / (stem + source.extension().string());
    }

    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
      error = "Could not copy " + source.filename().string() + " into the project: " + ec.message();
      return false;
    }
    tracks[i].relativePath = "audio/" + destination.filename().string();
  }

  nlohmann::json j;
  j["version"] = 1;
  j["name"] = name;

  nlohmann::json trackArray = nlohmann::json::array();
  for (const auto& track : tracks)
    trackArray.push_back({{"path", track.relativePath},
                          {"name", track.name},
                          {"gainDb", track.gainDb},
                          {"muted", track.muted},
                          {"soloed", track.soloed},
                          {"eqEnabled", track.eqEnabled},
                          {"eq", WriteEq(track.eq)},
                          {"noteTrack", WriteNotes(track.notes)}});
  j["tracks"] = trackArray;

  nlohmann::json loopArray = nlohmann::json::array();
  for (const auto& loop : loops)
    loopArray.push_back({{"name", loop.name}, {"start", loop.startSeconds}, {"end", loop.endSeconds}});
  j["loops"] = loopArray;
  j["activeLoop"] = activeLoop;

  j["gainDb"] = gainDb;
  j["speed"] = speed;
  j["semitones"] = semitones;
  j["positionSeconds"] = positionSeconds;

  j["tempoValid"] = tempoValid;
  j["bpm"] = bpm;
  j["firstBeatSeconds"] = firstBeatSeconds;
  j["confidence"] = confidence;
  j["beats"] = beats;
  j["downbeatOffset"] = downbeatOffset;
  j["lowestBpm"] = lowestBpm;
  j["highestBpm"] = highestBpm;
  j["beatsPerBar"] = beatsPerBar;
  j["showGrid"] = showGrid;
  j["snapToGrid"] = snapToGrid;

  j["viewStart"] = viewStart;
  j["viewDuration"] = viewDuration;
  j["followPlayhead"] = followPlayhead;

  j["metronome"] = config_json::WriteMetronome(metronome);
  j["clickArmed"] = clickArmed;

  const std::filesystem::path file = FileFor(target, name);
  std::ofstream out(file);
  if (!out.is_open())
  {
    error = "Could not write " + file.string();
    return false;
  }
  out << j.dump(2);
  if (!out.good())
  {
    error = "Could not finish writing " + file.string();
    return false;
  }

  // A project opened under the old extension and saved again would otherwise leave the old file
  // beside the new one, and a folder with two project files in it is a question nobody wants to be
  // asked. Removed only once the new one is safely written.
  std::filesystem::path superseded = file;
  superseded.replace_extension(kLegacyExtension);
  std::error_code removeError;
  std::filesystem::remove(superseded, removeError);

  return true;
}

bool ProjectFile::Load(const std::filesystem::path& file, ProjectFile& out, std::string& error)
{
  std::ifstream in(file);
  if (!in.is_open())
  {
    error = "Could not open " + file.string();
    return false;
  }

  try
  {
    nlohmann::json j;
    in >> j;

    out = ProjectFile();
    out.folder = file.parent_path();
    out.name = j.value("name", file.stem().string());

    if (j.contains("tracks") && j["tracks"].is_array())
    {
      for (const auto& stored : j["tracks"])
      {
        if (!stored.is_object())
          continue;
        TrackEntry track;
        track.relativePath = stored.value("path", std::string());
        track.name = stored.value("name", std::string());
        track.gainDb = stored.value("gainDb", track.gainDb);
        track.muted = stored.value("muted", track.muted);
        track.soloed = stored.value("soloed", track.soloed);
        track.eqEnabled = stored.value("eqEnabled", track.eqEnabled);
        if (stored.contains("eq"))
          track.eq = ReadEq(stored["eq"]);
        if (stored.contains("noteTrack"))
          track.notes = ReadNotes(stored["noteTrack"]);
        out.tracks.push_back(std::move(track));
      }
    }

    if (j.contains("loops") && j["loops"].is_array())
    {
      for (const auto& stored : j["loops"])
      {
        if (!stored.is_object())
          continue;
        LoopEntry loop;
        loop.name = stored.value("name", std::string("Loop"));
        loop.startSeconds = stored.value("start", loop.startSeconds);
        loop.endSeconds = stored.value("end", loop.endSeconds);
        out.loops.push_back(std::move(loop));
      }
    }
    out.activeLoop = j.value("activeLoop", out.activeLoop);

    out.gainDb = j.value("gainDb", out.gainDb);
    out.speed = j.value("speed", out.speed);
    out.semitones = j.value("semitones", out.semitones);
    out.positionSeconds = j.value("positionSeconds", out.positionSeconds);

    out.tempoValid = j.value("tempoValid", out.tempoValid);
    out.bpm = j.value("bpm", out.bpm);
    out.firstBeatSeconds = j.value("firstBeatSeconds", out.firstBeatSeconds);
    out.confidence = j.value("confidence", out.confidence);
    if (j.contains("beats") && j["beats"].is_array())
      for (const auto& beat : j["beats"])
        if (beat.is_number())
          out.beats.push_back(beat.get<double>());
    out.downbeatOffset = j.value("downbeatOffset", out.downbeatOffset);
    out.lowestBpm = j.value("lowestBpm", out.lowestBpm);
    out.highestBpm = j.value("highestBpm", out.highestBpm);
    out.beatsPerBar = j.value("beatsPerBar", out.beatsPerBar);
    out.showGrid = j.value("showGrid", out.showGrid);
    out.snapToGrid = j.value("snapToGrid", out.snapToGrid);

    out.viewStart = j.value("viewStart", out.viewStart);
    out.viewDuration = j.value("viewDuration", out.viewDuration);
    out.followPlayhead = j.value("followPlayhead", out.followPlayhead);

    if (j.contains("metronome") && j["metronome"].is_object())
      out.metronome = config_json::ReadMetronome(j["metronome"]);
    out.clickArmed = j.value("clickArmed", out.clickArmed);
  }
  catch (const std::exception& e)
  {
    error = std::string("That does not look like a project file: ") + e.what();
    return false;
  }
  return true;
}

std::vector<std::string> ProjectFile::List()
{
  std::vector<std::string> names;
  std::error_code ec;
  if (!std::filesystem::is_directory(DefaultFolder(), ec))
    return names;

  for (const auto& entry : std::filesystem::directory_iterator(DefaultFolder(), ec))
  {
    if (ec)
      break;
    if (!entry.is_directory(ec))
      continue;

    // A folder counts as a project when it has a project file in it, under either extension,
    // whatever else is in there.
    if (!FileIn(entry.path()).empty())
      names.push_back(entry.path().filename().string());
  }

  std::sort(names.begin(), names.end());
  return names;
}

} // namespace nam_ui
