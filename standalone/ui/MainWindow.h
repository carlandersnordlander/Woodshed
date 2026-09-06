#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "AudioEngine.h"
#include "CaptureLibrary.h"
#include "Config.h"
#include "DeepBeatTracker.h"
#include "NoteAnalysis.h"
#include "PolyphonicTranscriber.h"
#include "Project.h"
#include "StemSeparator.h"
#include "TempoAnalysis.h"
#include "Tone3000Client.h"

struct GLFWwindow;

namespace nam_ui
{

/// The app's modes. Browsing and tuning are activities of their own, so they get the whole window;
/// audio keeps running in every view and the bottom bar always shows the chain.
enum class View
{
  Rig,
  Player,
  Tuner,
  Metronome,
  Library,
  Settings
};

/// What the library view is showing. TONE3000 is a source alongside the local ones rather than a
/// separate destination - browsing is one activity.
enum class LibrarySource
{
  Rigs, ///< whole saved rigs, first because it is the one you reach for between songs
  All,
  Favorites,
  Recent,
  Cloud
};

/// Owns the app's state and draws the UI every frame. Constructed once after the GLFW/ImGui/GL
/// context is up; Draw() runs once per frame from main.cpp's loop.
class MainWindow
{
public:
  explicit MainWindow(GLFWwindow* window);
  ~MainWindow();

  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  void Draw();

private:
  /// The column of destinations down the left edge.
  void DrawNavRail();
  /// What this is and what it is built on, raised by the mark at the head of the rail.
  void DrawAboutPopup();
  bool mAboutOpen = false;
  void DrawSettingsView();

  void DrawRigView();
  /// The chain itself: the gate, then every block left to right, then the add button.
  void DrawBlockRail();
  /// Split mode, crossover frequency and branch blend for the selected parallel section.
  void DrawMergePanel();
  void SaveRoutingToConfig();

  /// Sentinels for mSelectedBlockId: a routing point rather than a block, with mSelectedSection
  /// saying which section it belongs to. Both open the same panel - split mode and blend are one
  /// setup - but each highlights its own handle.
  static constexpr int kMergePointSelection = -1;
  static constexpr int kSplitPointSelection = -2;
  /// The two ends of the chain: which interface input and output it runs between, and their trim.
  static constexpr int kInputSelection = -3;
  static constexpr int kOutputSelection = -4;
  /// The panel for one of those ends.
  void DrawEndpointPanel(bool isInput);
  /// The click's channel soloed: every player track silent, the metronome alone. Not stored with
  /// the tracks, because it is not one of them and letting go of it must not have disturbed any of
  /// their own mute and solo settings.
  bool mMetronomeSolo = false;
  /// Whether downloads finished while the Library view was not the one on screen, so the rail can
  /// say so. Cleared by going to the library. `mWasDownloading` is what spots the moment they
  /// stopped, since the client reports only whether any are running.
  bool mDownloadsWaiting = false;
  bool mWasDownloading = false;
  /// Until when the rig icon's cone rattles. A peak over the top lasts a handful of samples, so
  /// without a hold most of them would fall between two frames and never be drawn.
  double mClipUntil = 0.0;
  /// Whether this frame's click landed on a note or a chord and sounded it, so the timeline's seek
  /// leaves it alone - one press should mean one thing.
  bool mNoteAudition = false;
  /// The last place the playhead was put by hand. Stop returns here rather than to the top: you
  /// stop to play a passage again, and the passage is where you left the playhead.
  double mLastSeekSeconds = 0.0;
  /// Which folder the block panel's library is showing the contents of.
  std::string mBlockFolder;
  /// Which band of a parametric EQ block has its numbers open. One at a time: fifteen drag fields
  /// under the plot did not fit, and the plot already shows all five at once.
  size_t mSelectedEqBand = 0;
  /// Where a right-click on the chain asked for a block: the position along it, and which lane.
  /// Held because the menu opens a frame later, by which time the pointer has moved.
  int mAddAtIndex = 0;
  int mAddAtColumn = 0;
  bool mAddOnLowerLane = false;
  void DrawBlockPanel(int blockId);
  /// Removing a block throws away what is loaded in it, so it asks first.
  void DrawRemoveBlockPopup();
  /// The block's tone stack, as more rows in the same list as its levels.
  void DrawBlockEq(BlockSettings& settings, bool& settled, float labelWidth);
  /// The parametric EQ block: spectrum behind, response curve over it, one draggable handle per
  /// band. The spectrum is the amp's input, so a track's EQ asks for it without.
  /// \return true if a band changed
  bool DrawParametricEq(ParametricEqSettings& settings, float width, float height, bool showSpectrum = true);

  /// The spectrum, recomputed on the UI thread from what the engine tapped. Members rather than
  /// locals so the FFT scratch is allocated once instead of every frame.
  std::vector<float> mSpectrumSamples;
  std::vector<float> mSpectrumReal;
  std::vector<float> mSpectrumImag;
  /// Smoothed magnitudes in dB, one per FFT bin, so the display settles instead of flickering.
  std::vector<float> mSpectrumDb;
  /// Which band handle is being dragged, or -1.
  int mDraggedEqBand = -1;

  // --- the backing-track player -----------------------------------------------------------

  void DrawPlayerView();
  /// The stack of waveforms, the loop regions over them and the playhead. Also where clicking
  /// seeks and dragging marks out a new loop.
  ///
  /// \param laneHeight passed in rather than worked out here. Both panes used to derive it from
  ///        their own available space, and in "fit" mode those two numbers differ by a fraction of
  ///        a pixel - which is invisible on the first lane and half a row out by the eighth.
  void DrawTimeline(float width, float height, float laneHeight);
  void DrawTransportBar();
  /// The one header row: the project's name with everything you can do to it behind it, the two
  /// controls that get touched while playing, and what the timeline shows.
  void DrawProjectBar();
  /// The grid's tempo, in the corner above the track names, with its corrections behind it.
  void DrawTempoPanel(float width);
  /// The mixer and view keys that need the timeline on screen to mean anything.
  void HandlePlayerShortcuts();
  /// The transport's keys, which work in every view because the transport is in every view.
  void HandleTransportShortcuts();
  /// Opens a file dialog and adds whatever comes back as a new track.
  void AddPlayerTrack();
  /// Loads a decoded file as a track. Shared by the file dialog, finished stems and projects.
  /// \return the new track's id, or -1
  int AddPlayerTrackFromPath(const std::filesystem::path& path);

  /// The five bands of whichever track's EQ is open, over the top of everything else.
  void DrawTrackEqPopup();

  // --- projects --------------------------------------------------------------------------
  //
  // A project is a folder: the audio copied in beside a .namproj listing what was done with it.
  // Saving copies rather than references, so a project still opens after the stems it was built
  // from have been tidied away.

  void DrawSaveProjectPopup();
  /// Writes the Play view into `folder` under `name`, and adopts it as the open project.
  void SaveProjectTo(const std::filesystem::path& folder, const std::string& name);
  /// Replaces everything in the Play view with what the project holds.
  void OpenProject(const std::filesystem::path& file);
  /// Picks a project file and opens it.
  void BrowseAndOpenProject();
  /// Puts a project at the top of the recent list.
  void RememberProject(const std::filesystem::path& file);

  /// The .namproj currently open, or empty when nothing has been saved yet.
  std::filesystem::path mProjectFile;
  std::string mProjectName;
  /// Where a new project's folder is created. The default folder unless you pick another.
  std::filesystem::path mProjectParent;
  bool mSaveProjectDialogOpen = false;
  char mProjectNameInput[64] = {};

  /// Whether there is work here that saving would keep. Set wherever something is added, removed
  /// or changed; cleared on save and on open. Not perfect bookkeeping - it is a guard against
  /// throwing away an afternoon, so it errs towards asking.
  bool mProjectDirty = false;
  /// Set while the "start again" confirmation is up.
  bool mConfirmNewOpen = false;
  /// Clears the player and forgets the project, without asking.
  void StartNewProject();

  /// The options and the progress, both in one dialog: what you set is what the run does, and the
  /// run reports back in the same place you started it.
  void DrawSeparateStemsPopup();

  StemSeparator mSeparator;
  char mDemucsCommand[260] = {};
  bool mSeparateDialogOpen = false;

  /// The song's grid. Held here rather than in the player because nothing on the audio thread
  /// needs it - it draws lines and it snaps loop edges, and that is all.
  TempoAnalyser mTempoAnalyser;
  /// The deeper pass, for when the fast tracker counts the wrong pulse. It answers the same
  /// question and fills the same estimate, so nothing downstream knows which one found the beats.
  DeepBeatTracker mDeepBeats;
  /// Post-process the model with a dynamic Bayesian network: steadier on music that holds one
  /// tempo, slower to follow one that does not. Off by default, as its authors recommend.
  bool mDeepBeatsDbn = false;
  /// Starts it on the whole mix, summing the stems back together first when the project is in
  /// pieces - the model was trained on records, not on isolated tracks.
  void StartDeepBeats();
  TempoEstimate mTempo;
  bool mShowGrid = true;
  bool mSnapToGrid = true;
  int mGridBeatsPerBar = 4;
  /// How the grid is being counted against what was detected: half as often, as found, or twice.
  /// Shown as where you are rather than as two nudges, so getting back is a button rather than a
  /// memory of how many times each was pressed.
  float mGridMultiple = 1.0f;
  /// The playback speed the click was last set for, so a change to it can be noticed and the click
  /// re-joined at the new tempo.
  float mClickSpeed = 1.0f;
  /// Gives the metronome the song's tempo, scaled by the playback speed.
  void SendTempoToMetronome();

  /// Nearest beat to `seconds`, or `seconds` unchanged when there is no grid to snap to.
  double SnapToBeat(double seconds) const;

  /// What part of the track the timeline is showing. A duration of 0 means the whole thing.
  double mViewStart = 0.0;
  double mViewDuration = 0.0;

  /// How tall one track's lane is, in pixels. Zero means "share out whatever height there is",
  /// which is what it does until you zoom - the same convention mViewDuration uses for time.
  float mLaneHeight = 0.0f;
  /// How far the lanes are scrolled down, in pixels. One number for both panes, because the
  /// controls and the waveform they belong to must never be a pixel apart.
  float mLaneScroll = 0.0f;

  /// The track being dragged to a new position in the order, or 0.
  int mDraggingTrackId = 0;

  /// How tall each lane is right now: what you zoomed to, or an even share of the space when you
  /// have not. Asked in both panes so neither can work it out differently.
  float LaneHeight(size_t trackCount, float available) const;
  /// While playing, keep the playhead in view when zoomed in.
  bool mFollowPlayhead = true;

  /// Set while a loop is being dragged out, in seconds; -1 when nothing is being dragged.
  double mLoopDragStart = -1.0;
  double mLoopDragEnd = -1.0;

  /// What a drag on an existing loop is doing to it.
  enum class LoopEdit
  {
    None,
    Start,
    End,
    Move
  };
  LoopEdit mLoopEdit = LoopEdit::None;
  int mEditedLoop = -1;
  /// For a move: how far into the loop it was grabbed, so it does not jump to the pointer.
  double mLoopGrabOffset = 0.0;

  /// Which loop the list has highlighted, or -1.
  int mSelectedLoop = -1;
  char mLoopNameInput[48] = {};
  /// Which loop the ruler's right-click menu is about, or -1.
  int mLoopMenuIndex = -1;
  std::string mPlayerError;

  /// The track the mixer keys act on, or 0 for none.
  int mSelectedTrackId = 0;
  /// The track whose name is being typed, or 0. A lane is a label until you ask to change it.
  int mRenamingTrackId = 0;
  char mTrackNameInput[64] = {};
  /// Set when a rename starts, so the box takes the keyboard on the frame it appears and not on
  /// every frame after.
  bool mTrackRenameFocus = false;
  /// The track whose EQ window is open, or 0.
  int mEqTrackId = 0;

  /// What the note detector found, by track id. Held here rather than in the Track: the audio
  /// thread has no use for it, and a lane's notes are drawn, never played.
  std::unordered_map<int, NoteTrack> mTrackNotes;
  NoteAnalyser mNoteAnalyser;
  /// Chord names, found from the audio itself. Nothing to install, so it is the one of the three
  /// that always works.
  ChordAnalyser mChordAnalyser;
  /// The song's beats, for the chord analyser to measure between. Empty when there is no grid.
  std::vector<double> BeatTimes();

  /// Rebuilds what the synth plays from every track's notes and chords. Called whenever an
  /// analysis lands or a playback setting changes - never per frame.
  void RebuildSynthScore();
  /// The playback controls, shared by the note menu so there is one of them rather than three.
  /// \return true if something changed
  bool DrawPlaybackControls(NoteTrack& notes);
  /// The other way of filling a NoteTrack: several notes at once, out of a trained model.
  PolyphonicTranscriber mTranscriber;
  int mTranscribeTrackId = 0;
  bool mTranscribeDialogOpen = false;
  char mTranscribeCommand[260] = {};
  /// The options and the progress in one dialog, as with stem separation.
  void DrawTranscribePopup();
  /// Which Python to transcribe with: the one set in Settings, or an environment kept beside the
  /// app's own config, or nothing - which leaves the transcriber to try PATH.
  ///
  /// The middle case exists because basic-pitch needs TensorFlow, TensorFlow's Windows builds stop
  /// at Python 3.11, and the Python somebody already has is rarely that one. A private environment
  /// is the difference between the feature working and a wall of pip errors.
  std::string TranscribePython() const;
  /// Where that environment lives, whether or not it has been made.
  static std::filesystem::path ManagedPythonPath();

  /// Draws one track's notes under its waveform. Called from inside the timeline, which owns the
  /// mapping from seconds to pixels.
  void DrawNoteLane(const NoteTrack& notes, float top, float height, float left, float right, double viewStart,
                    double viewSpan, bool hovered);

  /// The click as a mode rather than a sound: armed here, running only while the song is.
  bool mClickArmed = false;

  /// How long the pointer has been off the master fader and its icon. A hover-opened panel that
  /// shuts the instant neither is hovered is one you cannot reach: the gap between them is not
  /// hovered either, and neither is the corner you cut across on the way.
  float mVolumeGrace = 0.0f;
  /// True while the transport is the thing running the metronome, so a click started from the
  /// metronome view is not stopped by a player that happens to be paused.
  bool mClickFromTransport = false;

  void DrawMetronomeView();
  /// The loaded preset's name, with saving, loading and deleting behind it.
  void DrawMetronomePresetMenu();
  /// The swinging pendulum. Its arc is driven by the beat phase, so it cannot drift from the click.
  void DrawPendulum(float width, float height, float phase, int beat, bool running);
  /// The other visual: the whole panel flares on the beat and fades away across it.
  void DrawBeatFlash(float width, float height, float phase, int beat, bool running);
  /// Pushes the current settings to the engine and remembers them in the config.
  void ApplyMetronome();
  /// Puts the click in step with the song at the playhead: same tempo, same place in the bar.
  void SyncMetronomeToSong();
  /// Last frame's playhead, for telling a seek apart from ordinary playback.
  double mLastPlayerPosition = 0.0;
  /// Where the click was last put back onto the beat, so a drifting song gets corrected steadily
  /// rather than only when the playhead jumps.
  double mLastClickSync = 0.0;

  void DrawTunerView();
  void UpdateTunerReference();
  /// Moves the reference onto whatever is being played, while auto is on.
  void FollowDetectedNote();
  /// Switches between following the played note and holding the chosen one, and remembers which.
  void SetTunerAuto(bool automatic);

  void DrawLibraryView();
  void DrawLibraryFilters();
  void DrawLibraryList();
  /// Everything known about the selected capture, and everything you can do to it.
  void DrawCaptureDetails();
  /// The actions a capture has, shared by its right-click menu and the details pane so the two
  /// can never drift apart.
  void DrawCaptureActions(const CaptureEntry& entry, bool asMenu);
  /// Deleting a file off the disk is not undoable, so it asks first.
  void DrawRemoveCapturePopup();
  /// One collapsible filter group: a search box, then its values in alphabetical order.
  /// \return true if a selection changed
  bool DrawFilterSection(const char* label, const std::vector<std::string>& values, std::vector<std::string>& target,
                         char* searchBuffer, size_t searchSize);
  /// The active filters, as chips you can click to drop. Without it, a filtered-down library looks
  /// like an empty one.
  /// \return true if something was dropped
  bool DrawFilterSummary(const std::vector<std::vector<std::string>*>& groups);
  /// Which block a click in the library lands in.
  void DrawBlockTargetSelector();
  void DrawCloudFilters();
  void DrawCloudResults();
  /// The selected search result, alongside what the library shows for a local file.
  void DrawToneDetails();


  /// Fills the screen the window is currently on, or puts it back where it was.
  void SetFullscreen(bool fullscreen);

  void HandleShortcuts();
  void RefreshDeviceLists();
  void ApplyDeviceSelection();

  // --- chain editing ---------------------------------------------------------------------

  int AddBlock(BlockType type);
  void RemoveBlock(int blockId);
  /// Queues a file for a block, choosing NAM or IR from its extension.
  void LoadFileIntoBlock(int blockId, const std::filesystem::path& path);
  /// Sends a library entry to the block that suits it, creating one if the chain has none.
  void LoadLibraryEntry(const CaptureEntry& entry);
  /// Hands finished loads to the engine.
  void PumpLoads();
  /// Reloads everything in the chain, e.g. after the stream settings changed.
  void ReloadChain();

  void SaveChainToConfig();
  void RestoreChainFromConfig();

  // --- rigs ------------------------------------------------------------------------------

  /// The saved rigs, and what you can do with them.
  void DrawRigList();
  /// The loaded rig's name, with saving, loading and deleting behind it.
  void DrawRigMenu();
  /// Confirms deleting a rig. Drawn at the top level, since either the rig menu or the library
  /// panel can ask for it.
  void DrawDeleteRigPopup();
  /// Builds a rig from what is loaded right now and writes it under `name`, replacing any rig of
  /// that name.
  void SaveRig(const std::string& name);
  /// Replaces the whole chain with a saved one.
  void LoadRig(const std::string& name);
  /// Rebuilds the chain from stored blocks and routing. Shared by rig loading and startup, so a
  /// rig and a restored session cannot end up meaning different things.
  void ApplyChain(const std::vector<AppConfig::BlockConfig>& blocks,
                  const std::vector<AppConfig::SectionConfig>& sections);

  /// Cached so the folder is not listed every frame; refreshed when a rig is written or removed.
  std::vector<std::string> mRigNames;
  bool mRigNamesLoaded = false;
  /// The rig currently loaded, for the list to mark and for Save to default to.
  std::string mCurrentRigName;
  char mRigNameInput[64] = {};
  /// Non-empty while the delete confirmation is open.
  std::string mPendingDeleteRig;

  void ApplyLoudnessMatch();

  /// The capture paths currently listed, rebuilt each frame; keyboard navigation walks this.
  std::vector<std::filesystem::path> mVisiblePaths;

  GLFWwindow* mWindow;
  AudioEngine mEngine;
  CaptureLibrary mCaptureLibrary;
  AppConfig mConfig;
  Tone3000Client mTone3000;

  /// The block being edited, and the one the library loads into. 0 means the gate is selected.
  int mSelectedBlockId = 0;
  int mSelectedIndex = 0;
  /// Which parallel section the routing panel refers to, when a split or merge point is selected.
  size_t mSelectedSection = 0;
  /// The block currently being dragged along the rail, or 0.
  int mDraggingBlockId = 0;
  /// Non-zero while the remove confirmation is open, holding the block it refers to.
  int mPendingRemoveBlockId = 0;

  /// What each block has loaded, and what went wrong if anything did. The engine holds the
  /// processors; these are only for showing the user.
  std::unordered_map<int, std::filesystem::path> mBlockPaths;
  std::unordered_map<int, std::string> mBlockErrors;

  View mView = View::Rig;
  LibrarySource mSource = LibrarySource::All;
  char mCaptureFilterText[64] = {};
  /// What a local file must match to be listed, all drawn from the downloaded metadata.
  std::vector<std::string> mLocalTagFilter;
  std::vector<std::string> mLocalGearFilter;
  std::vector<std::string> mLocalMakeFilter;
  std::vector<std::string> mLocalCreatorFilter;

  /// Per-section search boxes. A filter list with two hundred creators in it is not a list you
  /// scroll, it is one you search.
  char mLocalTagSearch[48] = {};
  char mLocalGearSearch[48] = {};
  char mLocalMakeSearch[48] = {};
  char mLocalCreatorSearch[48] = {};

  /// Set by the expand/collapse-all buttons and consumed by the next frame's headers: +1 opens
  /// every section, -1 closes them, 0 leaves each as the user left it.
  int mFilterExpandRequest = 0;
  int mGroupExpandRequest = 0;

  /// The capture the details pane is showing, by path so it survives a rescan - an index into the
  /// filtered list would point at a different file the moment the filter changed.
  std::filesystem::path mSelectedCapturePath;
  /// The file the delete confirmation refers to; empty when it is closed.
  std::filesystem::path mPendingDeletePath;
  char mNewTagText[48] = {};
  /// Search box for the cut-down library inside a block's panel.
  char mBlockPickerText[48] = {};

  std::vector<RtAudio::Api> mCompiledApis;
  int mSelectedApiIndex = 0;
  std::vector<RtAudio::DeviceInfo> mInputDevices;
  std::vector<RtAudio::DeviceInfo> mOutputDevices;
  int mSelectedInputIndex = -1;
  int mSelectedOutputIndex = -1;
  /// Which socket on the chosen interface: the input's one channel, and the first of the output's
  /// pair. Clamped against the device whenever the stream is opened.
  int mSelectedInputChannel = 0;
  int mSelectedOutputChannel = 0;
  int mSelectedSampleRateIndex = 1;
  int mBufferFramesValue = 256;

  bool mFullscreen = false;
  /// Where the window was before it went fullscreen, so it lands back exactly there.
  int mWindowedX = 0;
  int mWindowedY = 0;
  int mWindowedWidth = 0;
  int mWindowedHeight = 0;

  bool mAutoNormalize = true;
  std::filesystem::path mLastMatchedCapture;

  int mTunerNoteIndex = 9;
  int mTunerOctave = 2;
  float mTunerA4Hz = 440.0f;
  bool mTunerMutesOutput = true;

  /// \brief The tuner picks the note itself unless you lock one.
  ///
  /// Which is what tuning actually is: you play a string and want to know about that string. Being
  /// made to say which one first is a step that exists only because the strobe has to be told what
  /// to compare against.
  bool mTunerAuto = true;
  /// The note auto is considering moving to, and for how many readings running it has said so. A
  /// single reading is not enough - a struck string wobbles through a semitone or two before it
  /// settles, and following that would make the display flicker between neighbours.
  int mTunerCandidateNote = -1;
  int mTunerCandidateOctave = 0;
  int mTunerCandidateCount = 0;

  /// \brief Notes put over the top of the chosen tuning, by holding a string down and picking one.
  ///
  /// MIDI numbers, one per string, meaningful only while `mTunerCustomStrings` is set. Anything
  /// that changes which strings there are - the instrument, the count, the tuning - clears it,
  /// because an override is written against a position in a particular tuning.
  bool mTunerCustomStrings = false;
  std::array<int, 8> mTunerStringMidi{};
  /// Which string's note picker is open, or -1. Also marks the press as a hold, so letting go does
  /// not additionally read as a click on the string.
  int mTunerEditingString = -1;

  /// The metronome as the UI holds it, mirrored into the engine whenever it changes.
  AppConfig::MetronomePreset mMetronome;
  bool mMetronomeRunning = false;
  int mMetronomeVisual = 0;
  char mMetronomePresetName[64] = {};
  /// How tall everything under the visual came out last frame. The view does not scroll, so the
  /// visual has to be given exactly what is left over - and what is left over cannot be known
  /// until the controls have been drawn. Measuring the previous frame is a frame behind only on
  /// the first one, since the control block is a fixed set of rows.
  float mMetronomeControlsHeight = 0.0f;
  /// Tap tempo: when the last taps landed, oldest first. Cleared when they get too far apart to
  /// be the same tempo.
  std::vector<double> mTapTimes;
  /// A needle instead of the strobe. Coarser, but readable at a glance.
  bool mTunerNeedleMode = false;
  /// Where the needle is drawn, as opposed to what the tuner currently reads. A real needle has
  /// mass; this is what stands in for it.
  float mNeedleCents = 0.0f;
  /// Which tuning's strings are offered as lock buttons. Stored by name rather than by index, so
  /// adding a tuning to the table never silently changes what a saved config means.
  int mTunerInstrument = 0;
  int mTunerStringCount = 6;
  std::string mTunerTuningName = "Standard";

  SearchFilters mFilters;
  char mSearchQuery[128] = {};
  char mClientIdInput[128] = {};
  char mTagFilterText[64] = {};
  char mMakeFilterText[64] = {};
  char mCreatorFilterText[64] = {};
  /// The refresh token as last written to disk, so a new one - from a sign-in, or from the server
  /// rotating it - is noticed and persisted without re-encrypting on every frame.
  std::string mPersistedRefreshToken;
  bool mVerifiedCreatorsOnly = false;
  /// The search result the details pane is showing, or 0.
  long long mSelectedToneId = 0;
  bool mCloudListsLoaded = false;
  /// Set when a filter changes; the search fires on the next frame the client is free, so
  /// rapidly toggling several filters coalesces into one request instead of being dropped.
  bool mSearchPending = false;
};

} // namespace nam_ui
