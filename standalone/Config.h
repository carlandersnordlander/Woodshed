#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nam_ui
{

// Persisted app state, stored as JSON under %APPDATA%/Woodshed/config.json.
struct AppConfig
{
  std::string captureFolder;
  std::string apiName;
  std::string inputDeviceName;
  std::string outputDeviceName;
  unsigned int sampleRate = 48000;
  unsigned int bufferFrames = 256;
  float inputGainDb = 0.0f;
  float outputGainDb = 0.0f;
  bool bypassed = false;
  bool autoNormalize = true;
  /// Restored on startup: an app you left filling the screen should come back that way.
  bool fullscreen = false;

  /// How to reach Demucs for stem separation. Empty means "demucs", found on PATH.
  std::string demucsCommand;
  std::string demucsModel = "htdemucs";
  /// Empty for a full split; "vocals", "bass", "drums" or "other" for that stem plus the rest.
  std::string demucsTwoStems;
  /// Empty lets Demucs choose; "cpu" or "cuda" to insist.
  std::string demucsDevice;
  int demucsShifts = 0;
  /// Where stems are written. Empty puts them beside the app's config.
  std::string stemsFolder;

  /// The Python that runs basic-pitch for chord transcription. Empty means the one on PATH.
  std::string transcribeCommand;
  /// How sure the model has to be that a note started, and that it is continuing.
  float transcribeOnset = 0.5f;
  float transcribeFrame = 0.3f;
  /// Notes shorter than this are dropped, in milliseconds.
  float transcribeMinNoteMs = 128.0f;
  /// The range to look in, in Hz. Zero for either leaves it to the model.
  float transcribeMinHz = 0.0f;
  float transcribeMaxHz = 0.0f;
  /// TONE3000 publishable key (the OAuth client_id). Not a secret - it identifies the app, it does
  /// not authorise anything - so it is stored as it is.
  std::string tone3000ClientId;
  /// The OAuth refresh token, DPAPI-encrypted and base64-encoded. This one is a credential, and it
  /// never touches the disk in plaintext.
  std::string tone3000RefreshTokenEncrypted;

  /// A block's tone stack. Mirrors nam_ui::EqSettings without depending on the audio headers.
  struct StageEqConfig
  {
    bool enabled = false;
    int placement = 1; ///< 0 = pre (before the block's processor), 1 = post
    float lowDb = 0.0f;
    float midDb = 0.0f;
    float highDb = 0.0f;
    float midHz = 700.0f;
  };

  /// One unit of the chain, in order. Mirrors nam_ui::BlockSettings plus what is loaded into it.
  struct BlockConfig
  {
    int type = 0; ///< 0 = NAM capture, 1 = cab impulse response, 2 = low/high cut, 3 = compressor, 4 = parametric EQ
    std::string path;
    bool enabled = true;
    float gainDb = 0.0f;
    float levelDb = 0.0f;
    float dryBlend = 0.0f;
    /// Cut blocks only; each defaults to the end of its travel that does nothing.
    float lowCutHz = 20.0f;
    float highCutHz = 20000.0f;
    float compPeak = 5.0f;
    bool compLimit = false;

    /// EQ blocks only: five bands, each with enable, frequency, gain and Q.
    struct EqBandConfig
    {
      bool enabled = true;
      float hz = 1000.0f;
      float gainDb = 0.0f;
      float q = 1.0f;
    };
    std::vector<EqBandConfig> peqBands;

    int row = 0; ///< 0 = upper branch, 1 = lower
    StageEqConfig eq;
  };
  std::vector<BlockConfig> blocks;

  /// One parallel section: the blocks in [splitIndex, mergeIndex) run on two branches at once.
  /// mode: 0 = full signal to both branches, 1 = crossover.
  struct SectionConfig
  {
    int splitIndex = 0;
    int mergeIndex = 0;
    int mode = 0;
    float crossoverHz = 500.0f;
    /// The old single balance. Still read, so a chain saved before the two levels existed comes
    /// back sounding the same; written as -1 to mean "the levels below are the real values".
    float mix = 0.5f;
    float upperDb = 0.0f;
    float lowerDb = 0.0f;
    float mergeLevelDb = 0.0f;
  };
  /// The chain's parallel sections, in order. Empty means plain series.
  std::vector<SectionConfig> sections;

  /// Noise gate. placement: 0 = before the chain, 1 = after it.
  bool gateEnabled = false;
  int gatePlacement = 0;
  float gateThresholdDb = -60.0f;

  /// Tuner: the locked reference note (0 = C) and octave in scientific pitch notation, the
  /// concert-pitch calibration, and whether opening the tuner silences the output.
  int tunerNoteIndex = 9; ///< A
  int tunerOctave = 2;
  float tunerA4Hz = 440.0f;
  /// Which socket on the interface: the input's channel, and the first of the output's pair.
  int inputChannel = 0;
  int outputChannel = 0;

  /// The rig that was loaded when the app was last closed, so it comes back with it. Empty when
  /// the chain was never saved under a name - the chain itself is stored either way.
  std::string currentRigName;

  bool tunerMutesOutput = true;
  bool tunerNeedleMode = false;
  /// The tuner names the note itself unless a string has been held.
  bool tunerAuto = true;
  /// Notes put over the top of the chosen tuning, as MIDI numbers low string first. Empty when the
  /// tuning is being used as it comes.
  std::vector<int> tunerCustomStrings;
  /// The tuning whose strings are offered as lock buttons: instrument (0 = guitar, 1 = bass),
  /// string count, and the tuning's name.
  int tunerInstrument = 0;
  int tunerStringCount = 6;
  std::string tunerTuningName = "Standard";

  /// A metronome setting worth coming back to. Small enough to live in the config rather than
  /// getting a file format of its own.
  struct MetronomePreset
  {
    std::string name;
    float bpm = 120.0f;
    int beatsPerBar = 4;
    int beatUnit = 4;
    int subdivision = 1;
    int accentVoice = 0;
    int beatVoice = 0;
    int subVoice = 2;
    float accentDb = 0.0f;
    float beatDb = -4.0f;
    float subDb = -12.0f;
    bool subEnabled = true;
    float levelDb = -6.0f;
  };
  std::vector<MetronomePreset> metronomePresets;
  /// The metronome as it was last left, so it comes back the way you had it.
  MetronomePreset metronome;
  /// 0 = pendulum, 1 = flash.
  int metronomeVisual = 0;

  std::vector<std::string> favorites;
  /// Most-recently loaded first, capped at kMaxRecent.
  std::vector<std::string> recent;
  /// Projects opened or saved, most recent first. Full paths to the project file.
  std::vector<std::string> recentProjects;

  /// Tags the user attached to individual files, keyed by full path. Kept here rather than written
  /// into the downloaded sidecar, so what a capture arrived from TONE3000 with stays intact and
  /// distinguishable from what you added to it afterwards.
  std::map<std::string, std::vector<std::string>> userTags;

  static constexpr size_t kMaxRecent = 25;

  bool IsFavorite(const std::string& path) const;
  void ToggleFavorite(const std::string& path);
  void NoteRecent(const std::string& path);

  /// \brief The folder everything the app owns lives in: settings, rigs, projects, stems, the
  ///        Python it manages.
  ///
  /// Worked out once, on first use. That first call is also where an installation left under the
  /// app's previous name is moved across, so nothing else in the app has to know the app was ever
  /// called anything else.
  static std::filesystem::path GetConfigFolder();
  static std::filesystem::path GetConfigPath();
  static AppConfig Load();
  void Save() const;
};

/// \brief A saved rig: the chain, how it is wired, and the gate.
///
/// Deliberately not the audio device, the buffer size or the input and output gains. Those belong
/// to the room you are in rather than to the sound, and a rig that moved them would change how
/// loud you are every time you switched.
struct RigFile
{
  std::string name;
  std::vector<AppConfig::BlockConfig> blocks;
  std::vector<AppConfig::SectionConfig> sections;
  bool gateEnabled = false;
  int gatePlacement = 0;
  float gateThresholdDb = -60.0f;

  /// Where rigs are kept: %APPDATA%/Woodshed/rigs.
  static std::filesystem::path Folder();
  /// The file a rig with this name is saved to, with anything unusable stripped out.
  static std::filesystem::path PathFor(const std::string& name);
  /// Every saved rig, by name.
  static std::vector<std::string> List();

  bool Save() const;
  static bool Load(const std::string& name, RigFile& out);
  static bool Remove(const std::string& name);
};

} // namespace nam_ui

