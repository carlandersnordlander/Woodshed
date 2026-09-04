<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="design/woodshed-lockup-horisontell.svg">
    <img src="design/woodshed-lockup-horisontell-light.svg" alt="Woodshed" width="420">
  </picture>
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

If you already have Visual Studio and CMake, it is two commands from the repository root:

```
cmake -B build -S .
cmake --build build --config Release --target nam_standalone
```

Everything links statically, so the result is a single file you can copy anywhere. Dependencies are
fetched by CMake as pinned source archives with checked hashes — `git` is not needed for the build
and nothing has to be downloaded by hand.

<details>
<summary><b>Never built a C++ program before? Open this.</b></summary>

Nothing below needs any programming. You are downloading a compiler, pointing it at this folder and
letting it work. Windows only — the app uses Windows sound and Windows dialogs, so it will not build
on a Mac.

### 1. Install Visual Studio

Go to [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/) and get
**Visual Studio Community**. It is free. Do not get "Visual Studio Code" — that is a different
program with a similar name and it will not do this.

Run the installer. It shows a page of large tiles called *Workloads*. Tick the one that says
**Desktop development with C++**, then press **Install**. It is a big download — expect twenty
minutes or more, and it may want to restart the machine.

That one tick is the whole reason for this step: it installs the C++ compiler and a copy of CMake,
which is what actually builds the app.

### 2. Get the code

On the GitHub page, press the green **Code** button, then **Download ZIP**.

Then find the ZIP in your Downloads folder, **right-click it → Extract All…**, and pick somewhere
easy like your Desktop. Do not skip this: if you just double-click the ZIP, Windows shows you what
is inside without actually unpacking it, and the build will fail in a confusing way.

You now have a folder. Keep its window open — you need the path to it in a moment.

### 3. Open the right terminal

Press the **Start** button and type `Developer PowerShell`. Open the entry that comes up — it will
have a year in its name, like *Developer PowerShell for VS 2022*.

It has to be that one, not the ordinary PowerShell or Command Prompt. The Developer one knows where
the compiler is; the others do not.

### 4. Go to the folder

In the folder window from step 2, click once in the address bar at the top. The path turns into
text like `C:\Users\You\Desktop\Woodshed-main`. Copy it.

In the terminal, type `cd `, then a space, then paste the path in quotes, and press Enter:

```
cd "C:\Users\You\Desktop\Woodshed-main"
```

If it worked, the text to the left of the cursor changes to that folder. If it says *cannot find the
path*, the path is wrong — copy it again from the address bar.

### 5. Build it

Type this and press Enter:

```
cmake -B build -S .
```

It prints a wall of text and takes fifteen seconds or so. This is it downloading the libraries the
app needs and working out how to build. The last lines should say `Configuring done` and
`Generating done`.

Then type this and press Enter:

```
cmake --build build --config Release --target nam_standalone
```

This is the actual compiling. It prints file names as it goes and there will be yellow warning text
— that is normal, warnings are not errors. It took under two minutes on a fast machine; on a slow
laptop give it ten and let it finish.

The last line tells you where the program is:

```
nam_standalone.vcxproj -> ...\build\standalone\Release\Woodshed.exe
```

### 6. Run it

In the folder from step 2, open `build`, then `standalone`, then `Release`, and double-click
**Woodshed.exe**. You can also drag it to your Desktop — it is one self-contained file and it
works from anywhere.

### 7. Make it hear something

The app opens with no sound set up. Click the **cog** at the bottom of the strip down the left, to
get to Settings.

Under **AUDIO DEVICE**:

- **API** — pick `ASIO` if you have an audio interface with its own driver, otherwise `WASAPI`.
- **Input** — your audio interface, or your microphone if you have no interface.
- **Output** — your headphones or speakers.
- Leave the sample rate and buffer size alone to start with.
- Press **Apply and open**.

Just above the cog are two round knobs, for what goes in and what comes out. The ring around the
input knob fills up when sound reaches the app. If it moves when you make a noise, it is working.

**Use headphones.** With a microphone as the input and speakers as the output, the sound goes round
in a circle and howls.

### 8. Hear a capture

Still in Settings, under **CAPTURE FOLDER**, press **Choose folder** and pick the `example_models`
folder inside the folder from step 2. Those are test captures that come with the code.

Now click the **square with a circle in it** in the left strip to get to the Rig. The first time you
run it, the chain already has two blocks in it: one for an amp capture and one for a speaker
cabinet. Click the first one, then pick a capture from the list that appears below. Play, and it
comes out sounding like whatever that capture is.

You need an audio interface and a jack lead to plug a guitar in. Without one you can still use the
player, the metronome and the tuner — those only need headphones and a song.

### If something goes wrong

**`cmake : The term 'cmake' is not recognized`**
You are in the wrong terminal. Close it and go back to step 3 — it must be the *Developer*
PowerShell. If that still does not work, install CMake from
[cmake.org/download](https://cmake.org/download/) and tick *Add CMake to the system PATH* during
its install, then any terminal will do.

**`No CMAKE_CXX_COMPILER could be found`**
Visual Studio is installed but without the C++ part. Open *Visual Studio Installer* from the Start
menu, press **Modify**, tick **Desktop development with C++**, and let it finish.

**Windows says the app is blocked, or nothing happens when you double-click it**
Windows has a feature called Smart App Control that blocks programs it has not seen before, and a
program you just compiled yourself is by definition one of those. Building again is often enough —
the file changes and it gets through. If it keeps happening, **ask Anders before changing anything**:
switching Smart App Control off cannot be undone without reinstalling Windows.

**The app opens but the device lists are empty**
On ASIO that usually means your interface's driver is not installed. Switch API to `WASAPI` and try
again — it always has something. [`standalone/README.md`](standalone/README.md) has more on this.

**Red text in the build output**
Copy the first red line — the first one, not the last — and send it to Anders. Later errors are
usually just consequences of the first.

</details>

[`standalone/README.md`](standalone/README.md) covers the rest: audio setup in detail, the TONE3000
sign-in, the Python side for stems and transcription, and what to do when the ASIO device list comes
up empty.

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
