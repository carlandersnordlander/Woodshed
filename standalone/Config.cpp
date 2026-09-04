#include "Config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include "ConfigJson.h"
// For the project extension, which the one-time move renames files to. Project.h includes this
// header in turn, which the include guard settles.
#include "Project.h"
#include "json.hpp"

namespace nam_ui
{

namespace
{
constexpr const char* kConfigDirName = "Woodshed";
/// What the folder was called before the app was named. Kept so an existing installation is moved
/// across rather than abandoned, and for no other purpose.
constexpr const char* kLegacyConfigDirName = "NAMStandaloneUI";

constexpr const char* kConfigFileName = "config.json";

/// \brief Replaces one word with another throughout a text file, in place.
///
/// \return true when the file was read, changed and written back
bool ReplaceInFile(const std::filesystem::path& file, const std::string& from, const std::string& to)
{
  std::string text;
  {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open())
      return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  bool changed = false;
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
  {
    text.replace(at, from.size(), to);
    changed = true;
  }

  if (!changed)
    return false;

  std::ofstream out(file, std::ios::binary);
  if (!out.is_open())
    return false;
  out << text;
  return out.good();
}

/// \brief Points the settings under `root` at the folder's new name.
///
/// Paths that lead back inside the app's own folder are stored absolute - the managed Python, the
/// stems folder, the projects on the recent list - and after the folder moves every one of them
/// leads nowhere. Done as a text replacement of the folder's name rather than by reading each file
/// as JSON and walking it: the old name is distinctive enough that nothing else can match it, and
/// one pass catches both slash styles and JSON's escaped backslashes, which parsing paths would
/// have to handle case by case.
///
/// Only the files that hold settings are touched. Walking the whole tree would mean reading every
/// file of the Python environment, which is tens of thousands of them and none of them ours.
void RewriteFolderName(const std::filesystem::path& root, const std::string& from, const std::string& to)
{
  std::error_code ec;

  ReplaceInFile(root / kConfigFileName, from, to);

  for (const auto& entry : std::filesystem::directory_iterator(root / "rigs", ec))
    if (!ec && entry.is_regular_file(ec))
      ReplaceInFile(entry.path(), from, to);

  ec.clear();
  for (const auto& project : std::filesystem::directory_iterator(root / "projects", ec))
  {
    if (ec || !project.is_directory(ec))
      continue;

    std::error_code inner;
    for (const auto& file : std::filesystem::directory_iterator(project.path(), inner))
    {
      if (inner || !file.is_regular_file(inner))
        continue;
      const std::filesystem::path extension = file.path().extension();
      if (extension == ProjectFile::kExtension || extension == ProjectFile::kLegacyExtension)
        ReplaceInFile(file.path(), from, to);
    }
  }
}

/// Gives the projects in `folder` the extension the app writes now. The old one still opens, so
/// this is tidiness rather than necessity - but a folder of files named after a name the app no
/// longer answers to is a puzzle for whoever opens it next.
void RenameLegacyProjects(const std::filesystem::path& folder)
{
  std::error_code ec;
  for (const auto& project : std::filesystem::directory_iterator(folder, ec))
  {
    if (ec || !project.is_directory(ec))
      continue;

    std::error_code inner;
    for (const auto& file : std::filesystem::directory_iterator(project.path(), inner))
    {
      if (inner || !file.is_regular_file(inner) || file.path().extension() != ProjectFile::kLegacyExtension)
        continue;

      std::filesystem::path renamed = file.path();
      renamed.replace_extension(ProjectFile::kExtension);
      // Never over one that is already there: that one is the newer of the two.
      if (!std::filesystem::exists(renamed, inner))
        std::filesystem::rename(file.path(), renamed, inner);
    }
  }
}

std::vector<std::string> JsonStringArray(const nlohmann::json& j, const char* key)
{
  std::vector<std::string> out;
  if (j.contains(key) && j[key].is_array())
    for (const auto& entry : j[key])
      if (entry.is_string())
        out.push_back(entry.get<std::string>());
  return out;
}

// The chain is written by both the config and by every saved rig, so it is read and written in
// one place - otherwise a new block setting has to be remembered in four spots instead of two.

std::vector<AppConfig::BlockConfig> ReadBlocks(const nlohmann::json& j)
{
  std::vector<AppConfig::BlockConfig> blocks;
  if (!j.contains("blocks") || !j["blocks"].is_array())
    return blocks;

  for (const auto& entry : j["blocks"])
  {
    if (!entry.is_object())
      continue;
    AppConfig::BlockConfig block;
    block.type = entry.value("type", block.type);
    block.path = entry.value("path", block.path);
    block.enabled = entry.value("enabled", block.enabled);
    // "trimDb" is what the block's post-processor level used to be called, back when it was the
    // only one; read it so an existing chain keeps its levels.
    block.levelDb = entry.value("levelDb", entry.value("trimDb", block.levelDb));
    block.gainDb = entry.value("gainDb", block.gainDb);
    block.dryBlend = entry.value("dryBlend", block.dryBlend);
    block.lowCutHz = entry.value("lowCutHz", block.lowCutHz);
    block.highCutHz = entry.value("highCutHz", block.highCutHz);
    block.compPeak = entry.value("compPeak", block.compPeak);
    block.compLimit = entry.value("compLimit", block.compLimit);

    if (entry.contains("peqBands") && entry["peqBands"].is_array())
    {
      for (const auto& stored : entry["peqBands"])
      {
        if (!stored.is_object())
          continue;
        AppConfig::BlockConfig::EqBandConfig band;
        band.enabled = stored.value("enabled", band.enabled);
        band.hz = stored.value("hz", band.hz);
        band.gainDb = stored.value("gainDb", band.gainDb);
        band.q = stored.value("q", band.q);
        block.peqBands.push_back(band);
      }
    }

    block.row = entry.value("row", block.row);
    if (entry.contains("eq") && entry["eq"].is_object())
    {
      const auto& eq = entry["eq"];
      block.eq.enabled = eq.value("enabled", block.eq.enabled);
      block.eq.placement = eq.value("placement", block.eq.placement);
      block.eq.lowDb = eq.value("lowDb", block.eq.lowDb);
      block.eq.midDb = eq.value("midDb", block.eq.midDb);
      block.eq.highDb = eq.value("highDb", block.eq.highDb);
      block.eq.midHz = eq.value("midHz", block.eq.midHz);
    }
    blocks.push_back(std::move(block));
  }
  return blocks;
}

nlohmann::json WriteBlocks(const std::vector<AppConfig::BlockConfig>& blocks)
{
  nlohmann::json array = nlohmann::json::array();
  for (const auto& block : blocks)
  {
    nlohmann::json bands = nlohmann::json::array();
    for (const auto& band : block.peqBands)
      bands.push_back({{"enabled", band.enabled}, {"hz", band.hz}, {"gainDb", band.gainDb}, {"q", band.q}});

    array.push_back({{"type", block.type},
                     {"path", block.path},
                     {"enabled", block.enabled},
                     {"gainDb", block.gainDb},
                     {"levelDb", block.levelDb},
                     {"dryBlend", block.dryBlend},
                     {"lowCutHz", block.lowCutHz},
                     {"highCutHz", block.highCutHz},
                     {"compPeak", block.compPeak},
                     {"compLimit", block.compLimit},
                     {"peqBands", bands},
                     {"row", block.row},
                     {"eq",
                      {{"enabled", block.eq.enabled},
                       {"placement", block.eq.placement},
                       {"lowDb", block.eq.lowDb},
                       {"midDb", block.eq.midDb},
                       {"highDb", block.eq.highDb},
                       {"midHz", block.eq.midHz}}}});
  }
  return array;
}

std::vector<AppConfig::SectionConfig> ReadSections(const nlohmann::json& j)
{
  std::vector<AppConfig::SectionConfig> sections;
  if (!j.contains("sections") || !j["sections"].is_array())
    return sections;

  for (const auto& entry : j["sections"])
  {
    if (!entry.is_object())
      continue;
    AppConfig::SectionConfig section;
    section.splitIndex = entry.value("splitIndex", section.splitIndex);
    section.mergeIndex = entry.value("mergeIndex", section.mergeIndex);
    section.mode = entry.value("mode", section.mode);
    section.crossoverHz = entry.value("crossoverHz", section.crossoverHz);
    section.mergeLevelDb = entry.value("mergeLevelDb", section.mergeLevelDb);

    // Two levels when they are there; otherwise the old balance, turned into the pair of levels
    // that sounds the same. Equal parts was a mix of 0.5, which is both branches at unity.
    if (entry.contains("upperDb") || entry.contains("lowerDb"))
    {
      section.upperDb = entry.value("upperDb", section.upperDb);
      section.lowerDb = entry.value("lowerDb", section.lowerDb);
    }
    else
    {
      const float mix = std::clamp(entry.value("mix", 0.5f), 0.0f, 1.0f);
      section.upperDb = (mix >= 1.0f) ? -60.0f : 20.0f * std::log10(std::max(1.0e-3f, (1.0f - mix) * 2.0f));
      section.lowerDb = (mix <= 0.0f) ? -60.0f : 20.0f * std::log10(std::max(1.0e-3f, mix * 2.0f));
    }
    sections.push_back(section);
  }
  return sections;
}

nlohmann::json WriteSections(const std::vector<AppConfig::SectionConfig>& sections)
{
  nlohmann::json array = nlohmann::json::array();
  for (const auto& section : sections)
    array.push_back({{"splitIndex", section.splitIndex},
                     {"mergeIndex", section.mergeIndex},
                     {"mode", section.mode},
                     {"crossoverHz", section.crossoverHz},
                     {"upperDb", section.upperDb},
                     {"lowerDb", section.lowerDb},
                     {"mergeLevelDb", section.mergeLevelDb}});
  return array;
}
} // namespace

namespace config_json
{

AppConfig::MetronomePreset ReadMetronome(const nlohmann::json& j)
{
  AppConfig::MetronomePreset preset;
  preset.name = j.value("name", preset.name);
  preset.bpm = j.value("bpm", preset.bpm);
  preset.beatsPerBar = j.value("beatsPerBar", preset.beatsPerBar);
  preset.beatUnit = j.value("beatUnit", preset.beatUnit);
  preset.subdivision = j.value("subdivision", preset.subdivision);
  preset.accentVoice = j.value("accentVoice", preset.accentVoice);
  preset.beatVoice = j.value("beatVoice", preset.beatVoice);
  preset.subVoice = j.value("subVoice", preset.subVoice);
  preset.accentDb = j.value("accentDb", preset.accentDb);
  preset.beatDb = j.value("beatDb", preset.beatDb);
  preset.subDb = j.value("subDb", preset.subDb);
  preset.subEnabled = j.value("subEnabled", preset.subEnabled);
  preset.levelDb = j.value("levelDb", preset.levelDb);
  return preset;
}

nlohmann::json WriteMetronome(const AppConfig::MetronomePreset& preset)
{
  return {{"name", preset.name},
          {"bpm", preset.bpm},
          {"beatsPerBar", preset.beatsPerBar},
          {"beatUnit", preset.beatUnit},
          {"subdivision", preset.subdivision},
          {"accentVoice", preset.accentVoice},
          {"beatVoice", preset.beatVoice},
          {"subVoice", preset.subVoice},
          {"accentDb", preset.accentDb},
          {"beatDb", preset.beatDb},
          {"subDb", preset.subDb},
          {"subEnabled", preset.subEnabled},
          {"levelDb", preset.levelDb}};
}

} // namespace config_json

using config_json::ReadMetronome;
using config_json::WriteMetronome;

bool AppConfig::IsFavorite(const std::string& path) const
{
  return std::find(favorites.begin(), favorites.end(), path) != favorites.end();
}

void AppConfig::ToggleFavorite(const std::string& path)
{
  const auto it = std::find(favorites.begin(), favorites.end(), path);
  if (it == favorites.end())
    favorites.push_back(path);
  else
    favorites.erase(it);
}

void AppConfig::NoteRecent(const std::string& path)
{
  recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
  recent.insert(recent.begin(), path);
  if (recent.size() > kMaxRecent)
    recent.resize(kMaxRecent);
}

std::filesystem::path AppConfig::GetConfigFolder()
{
  // Worked out once. Everything downstream - rigs, projects, stems, the managed Python - is built
  // from this, so resolving it repeatedly would mean repeating the move below as well.
  static const std::filesystem::path folder = []()
  {
    const char* appData = std::getenv("APPDATA");
    const std::filesystem::path base =
      (appData != nullptr) ? std::filesystem::path(appData) : std::filesystem::current_path();

    const std::filesystem::path current = base / kConfigDirName;
    const std::filesystem::path legacy = base / kLegacyConfigDirName;

    std::error_code ec;
    if (std::filesystem::exists(current, ec) || !std::filesystem::is_directory(legacy, ec))
      return current;

    // --- an installation left under the app's old name ---
    //
    // A rename within one folder on one volume, so it is a single atomic operation rather than a
    // copy that can half-finish. The managed Python survives it: a virtual environment finds itself
    // from where its own executable sits, and its base interpreter is outside this folder anyway.
    std::filesystem::rename(legacy, current, ec);
    if (ec)
      return legacy; // something has it open: go on using what is there rather than starting blank

    // Projects first, then the settings: the rewrite below has to run over the files under their
    // final names, and the recent list is put right when the config is read.
    RenameLegacyProjects(current / "projects");
    RewriteFolderName(current, kLegacyConfigDirName, kConfigDirName);
    return current;
  }();

  return folder;
}

std::filesystem::path AppConfig::GetConfigPath()
{
  return GetConfigFolder() / kConfigFileName;
}

AppConfig AppConfig::Load()
{
  AppConfig config;
  std::ifstream in(GetConfigPath());
  if (!in.is_open())
    return config;

  try
  {
    nlohmann::json j;
    in >> j;
    config.captureFolder = j.value("captureFolder", config.captureFolder);
    config.apiName = j.value("apiName", config.apiName);
    config.inputDeviceName = j.value("inputDeviceName", config.inputDeviceName);
    config.outputDeviceName = j.value("outputDeviceName", config.outputDeviceName);
    config.sampleRate = j.value("sampleRate", config.sampleRate);
    config.bufferFrames = j.value("bufferFrames", config.bufferFrames);
    config.inputGainDb = j.value("inputGainDb", config.inputGainDb);
    config.outputGainDb = j.value("outputGainDb", config.outputGainDb);
    config.bypassed = j.value("bypassed", config.bypassed);
    config.autoNormalize = j.value("autoNormalize", config.autoNormalize);
    config.fullscreen = j.value("fullscreen", config.fullscreen);
    config.demucsCommand = j.value("demucsCommand", config.demucsCommand);
    config.demucsModel = j.value("demucsModel", config.demucsModel);
    config.demucsTwoStems = j.value("demucsTwoStems", config.demucsTwoStems);
    config.demucsDevice = j.value("demucsDevice", config.demucsDevice);
    config.demucsShifts = j.value("demucsShifts", config.demucsShifts);
    config.stemsFolder = j.value("stemsFolder", config.stemsFolder);
    config.transcribeCommand = j.value("transcribeCommand", config.transcribeCommand);
    config.transcribeOnset = j.value("transcribeOnset", config.transcribeOnset);
    config.transcribeFrame = j.value("transcribeFrame", config.transcribeFrame);
    config.transcribeMinNoteMs = j.value("transcribeMinNoteMs", config.transcribeMinNoteMs);
    config.transcribeMinHz = j.value("transcribeMinHz", config.transcribeMinHz);
    config.transcribeMaxHz = j.value("transcribeMaxHz", config.transcribeMaxHz);
    config.tone3000ClientId = j.value("tone3000ClientId", config.tone3000ClientId);
    config.tone3000RefreshTokenEncrypted =
      j.value("tone3000RefreshTokenEncrypted", config.tone3000RefreshTokenEncrypted);

    config.gateEnabled = j.value("gateEnabled", config.gateEnabled);
    config.gatePlacement = j.value("gatePlacement", config.gatePlacement);
    config.gateThresholdDb = j.value("gateThresholdDb", config.gateThresholdDb);

    config.tunerNoteIndex = j.value("tunerNoteIndex", config.tunerNoteIndex);
    config.tunerOctave = j.value("tunerOctave", config.tunerOctave);
    config.tunerA4Hz = j.value("tunerA4Hz", config.tunerA4Hz);
    config.inputChannel = j.value("inputChannel", config.inputChannel);
    config.outputChannel = j.value("outputChannel", config.outputChannel);
    config.currentRigName = j.value("currentRigName", config.currentRigName);
    config.tunerMutesOutput = j.value("tunerMutesOutput", config.tunerMutesOutput);
    config.tunerNeedleMode = j.value("tunerNeedleMode", config.tunerNeedleMode);
    config.tunerAuto = j.value("tunerAuto", config.tunerAuto);
    if (j.contains("tunerCustomStrings") && j["tunerCustomStrings"].is_array())
      config.tunerCustomStrings = j["tunerCustomStrings"].get<std::vector<int>>();
    config.tunerInstrument = j.value("tunerInstrument", config.tunerInstrument);
    config.tunerStringCount = j.value("tunerStringCount", config.tunerStringCount);
    config.tunerTuningName = j.value("tunerTuningName", config.tunerTuningName);

    if (j.contains("sections") && j["sections"].is_array())
    {
      config.sections = ReadSections(j);
    }
    else if (j.value("routingParallel", false))
    {
      // Written before the chain could hold more than one parallel section; carry the one it had
      // over rather than dropping the user's routing on the first launch after the change.
      SectionConfig section;
      section.splitIndex = j.value("routingSplitIndex", 0);
      section.mergeIndex = j.value("routingMergeIndex", 0);
      section.mode = j.value("routingMode", 0);
      section.crossoverHz = j.value("routingCrossoverHz", 500.0f);
      section.mergeLevelDb = j.value("routingMergeLevelDb", 0.0f);
      const float mix = std::clamp(j.value("routingMix", 0.5f), 0.0f, 1.0f);
      section.upperDb = (mix >= 1.0f) ? -60.0f : 20.0f * std::log10(std::max(1.0e-3f, (1.0f - mix) * 2.0f));
      section.lowerDb = (mix <= 0.0f) ? -60.0f : 20.0f * std::log10(std::max(1.0e-3f, mix * 2.0f));
      config.sections.push_back(section);
    }

    config.favorites = JsonStringArray(j, "favorites");
    config.recent = JsonStringArray(j, "recent");
    config.recentProjects = JsonStringArray(j, "recentProjects");

    // A project that was given the current extension - by the one-time move, or by being saved
    // again - is still on this list under the name it had. Followed to where it went rather than
    // left to show as missing, and only when the old file really has gone and the new one is
    // really there, so a project that was genuinely deleted still reads as deleted.
    for (auto& path : config.recentProjects)
    {
      std::filesystem::path file(path);
      if (file.extension() != ProjectFile::kLegacyExtension)
        continue;

      std::error_code ec;
      if (std::filesystem::exists(file, ec))
        continue;

      file.replace_extension(ProjectFile::kExtension);
      if (std::filesystem::exists(file, ec))
      {
        path = file.string();
        continue;
      }

      // Not under the same name, then. A project is a folder, and the file inside it can have been
      // renamed by hand, so the folder is asked what it holds.
      const std::filesystem::path inFolder = ProjectFile::FileIn(file.parent_path());
      if (!inFolder.empty())
        path = inFolder.string();
    }

    if (j.contains("userTags") && j["userTags"].is_object())
    {
      for (const auto& item : j["userTags"].items())
      {
        std::vector<std::string> tags;
        if (item.value().is_array())
          for (const auto& tag : item.value())
            if (tag.is_string())
              tags.push_back(tag.get<std::string>());
        if (!tags.empty())
          config.userTags.emplace(item.key(), std::move(tags));
      }
    }

    config.blocks = ReadBlocks(j);

    if (j.contains("metronome") && j["metronome"].is_object())
      config.metronome = ReadMetronome(j["metronome"]);
    config.metronomeVisual = j.value("metronomeVisual", config.metronomeVisual);
    if (j.contains("metronomePresets") && j["metronomePresets"].is_array())
      for (const auto& entry : j["metronomePresets"])
        if (entry.is_object())
          config.metronomePresets.push_back(ReadMetronome(entry));
  }
  catch (const std::exception&)
  {
    // Corrupt/unreadable config: fall back to defaults rather than failing startup.
    return AppConfig();
  }

  return config;
}

void AppConfig::Save() const
{
  const auto path = GetConfigPath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  nlohmann::json j;
  j["captureFolder"] = captureFolder;
  j["apiName"] = apiName;
  j["inputDeviceName"] = inputDeviceName;
  j["outputDeviceName"] = outputDeviceName;
  j["sampleRate"] = sampleRate;
  j["bufferFrames"] = bufferFrames;
  j["inputGainDb"] = inputGainDb;
  j["outputGainDb"] = outputGainDb;
  j["bypassed"] = bypassed;
  j["autoNormalize"] = autoNormalize;
  j["fullscreen"] = fullscreen;
  j["demucsCommand"] = demucsCommand;
  j["demucsModel"] = demucsModel;
  j["demucsTwoStems"] = demucsTwoStems;
  j["demucsDevice"] = demucsDevice;
  j["demucsShifts"] = demucsShifts;
  j["stemsFolder"] = stemsFolder;
  j["transcribeCommand"] = transcribeCommand;
  j["transcribeOnset"] = transcribeOnset;
  j["transcribeFrame"] = transcribeFrame;
  j["transcribeMinNoteMs"] = transcribeMinNoteMs;
  j["transcribeMinHz"] = transcribeMinHz;
  j["transcribeMaxHz"] = transcribeMaxHz;
  j["tone3000ClientId"] = tone3000ClientId;
  j["tone3000RefreshTokenEncrypted"] = tone3000RefreshTokenEncrypted;

  j["gateEnabled"] = gateEnabled;
  j["gatePlacement"] = gatePlacement;
  j["gateThresholdDb"] = gateThresholdDb;

  j["tunerNoteIndex"] = tunerNoteIndex;
  j["tunerOctave"] = tunerOctave;
  j["tunerA4Hz"] = tunerA4Hz;
  j["inputChannel"] = inputChannel;
  j["outputChannel"] = outputChannel;
  j["currentRigName"] = currentRigName;
  j["tunerMutesOutput"] = tunerMutesOutput;
  j["tunerNeedleMode"] = tunerNeedleMode;
  j["tunerAuto"] = tunerAuto;
  j["tunerCustomStrings"] = tunerCustomStrings;
  j["tunerInstrument"] = tunerInstrument;
  j["tunerStringCount"] = tunerStringCount;
  j["tunerTuningName"] = tunerTuningName;

  j["sections"] = WriteSections(sections);

  j["favorites"] = favorites;
  j["recent"] = recent;
  j["recentProjects"] = recentProjects;

  nlohmann::json tagMap = nlohmann::json::object();
  for (const auto& entry : userTags)
    if (!entry.second.empty())
      tagMap[entry.first] = entry.second;
  j["userTags"] = tagMap;

  j["blocks"] = WriteBlocks(blocks);

  j["metronome"] = WriteMetronome(metronome);
  j["metronomeVisual"] = metronomeVisual;
  nlohmann::json presetArray = nlohmann::json::array();
  for (const auto& preset : metronomePresets)
    presetArray.push_back(WriteMetronome(preset));
  j["metronomePresets"] = presetArray;

  std::ofstream out(path);
  if (out.is_open())
    out << j.dump(2);
}

std::filesystem::path RigFile::Folder()
{
  return AppConfig::GetConfigFolder() / "rigs";
}

std::filesystem::path RigFile::PathFor(const std::string& name)
{
  // The name is typed by hand and becomes a file name, so anything a path cannot carry is
  // replaced rather than rejected - being told off for a colon is not a useful experience.
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
    safe = "rig";

  return Folder() / (safe + ".json");
}

std::vector<std::string> RigFile::List()
{
  std::vector<std::string> names;
  std::error_code ec;
  if (!std::filesystem::is_directory(Folder(), ec))
    return names;

  for (const auto& entry : std::filesystem::directory_iterator(Folder(), ec))
  {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
      continue;

    // The name inside the file wins over the file name: it is what the user typed, punctuation
    // and all, while the file name has been through PathFor.
    RigFile rig;
    if (Load(entry.path().stem().string(), rig) && !rig.name.empty())
      names.push_back(rig.name);
    else
      names.push_back(entry.path().stem().string());
  }

  std::sort(names.begin(), names.end());
  return names;
}

bool RigFile::Save() const
{
  std::error_code ec;
  std::filesystem::create_directories(Folder(), ec);

  nlohmann::json j;
  j["name"] = name;
  j["blocks"] = WriteBlocks(blocks);
  j["sections"] = WriteSections(sections);
  j["gateEnabled"] = gateEnabled;
  j["gatePlacement"] = gatePlacement;
  j["gateThresholdDb"] = gateThresholdDb;

  std::ofstream out(PathFor(name));
  if (!out.is_open())
    return false;
  out << j.dump(2);
  return out.good();
}

bool RigFile::Load(const std::string& name, RigFile& out)
{
  std::ifstream in(PathFor(name));
  if (!in.is_open())
    return false;

  try
  {
    nlohmann::json j;
    in >> j;
    out.name = j.value("name", name);
    out.blocks = ReadBlocks(j);
    out.sections = ReadSections(j);
    out.gateEnabled = j.value("gateEnabled", out.gateEnabled);
    out.gatePlacement = j.value("gatePlacement", out.gatePlacement);
    out.gateThresholdDb = j.value("gateThresholdDb", out.gateThresholdDb);
  }
  catch (const std::exception&)
  {
    return false;
  }
  return true;
}

bool RigFile::Remove(const std::string& name)
{
  std::error_code ec;
  return std::filesystem::remove(PathFor(name), ec);
}

} // namespace nam_ui
