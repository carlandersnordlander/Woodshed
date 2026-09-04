<div align="center">
  <img src="design/woodshed-lockup-horisontell.svg" alt="Woodshed" width="420">
</div>

> *"Woodshedding", or shedding, is a term commonly used to describe the act of practicing some
> endeavor, usually in private, to improve one's proficiency in performing it. It is typically used
> by musicians to mean rehearsing a difficult passage repeatedly, until it can be performed
> flawlessly.*

A practice room for guitar and bass, as one Windows application. Play live through
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore) captures and cab impulse
responses at ASIO latency, and put a song on the other side of the same input — split into its
stems, with its tempo, its chords and its notes worked out for you.

Not a plugin. A single `.exe` with its own audio I/O and no host to load it into.

## What it does

**The rig.** A chain of blocks you drag into place: NAM captures, cab IRs, low/high cut,
compressor, five-band parametric EQ. Drop a block onto the lower lane and the signal splits, with
independent levels on each branch and a crossover if you want the split by frequency rather than in
full. Twenty-four blocks and twelve parallel sections, per-block tone stack placed before or after
the processor, and a noise gate whose detection always listens to the clean input so the threshold
keeps meaning something about how hard you played.

**The player.** Import a song and separate it into stems with [Demucs](https://github.com/adefossez/demucs).
Each stem is a channel with its own level, mute, solo and five-band EQ. Loop a passage by dragging
in the ruler, slow it down or change its key without changing the other, and step between the loops
you saved.

**What the song is doing.** Tempo and beat positions from
[Beat This!](https://github.com/CPJKU/beat_this), a transformer beat tracker, with bars drawn
against the waveform and a click that follows them — including through meter changes. Chord names
across the track from a chromagram, which needs nothing installed and works on a full mix. Single
notes from a YIN detector, or every note of every voice through
[basic-pitch](https://github.com/spotify/basic-pitch). Found notes can be played back on a small
synth, in whatever octave sits clear of the music, and clicking one plays it.

**The tuner.** A strobe and a needle, naming the note itself or locked to a string you pick, across
twelve keys of every tuning it knows, with adjustable concert pitch.

**The metronome.** Its own view, and a channel in the player against the song's own grid.

## Build

Windows, CMake 3.14+, Visual Studio 2022 with the C++ workload. From the repository root:

```
cmake -B build -S .
cmake --build build --config Release --target nam_standalone
```

Everything links statically, so the result is a single file you can copy anywhere. Dependencies are
fetched by CMake as pinned source archives with checked hashes — `git` is not needed for the build
and nothing has to be downloaded by hand.

[`standalone/README.md`](standalone/README.md) covers the rest: audio setup, the TONE3000 sign-in,
the Python side for stems and transcription, and what to do when the ASIO device list comes up
empty.

## What is in this repository

`standalone/` is Woodshed. Everything else is
[NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) by Steven Atkinson — the
DSP that loads and runs a `.nam` capture — carried here rather than fetched, with its licence,
its tools and its own README upstream. Woodshed compiles those sources directly; it does not
modify them, beyond a fallback in the root `CMakeLists.txt` that fetches Eigen when the submodule
is empty.

`example_models/` are upstream's test captures, and are the quickest way to hear the app do
anything. No other captures are included: a `.nam` downloaded from TONE3000 is somebody's work
under their terms, and this repository is not the place to republish it.

See [THIRD_PARTY.md](THIRD_PARTY.md) for every component and its licence.

## Licence

MIT. The NAM core is Copyright (c) 2023 Steven Atkinson under the same licence; see
[LICENSE](LICENSE), which covers both.
