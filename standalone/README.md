# Woodshed

A practice room for guitar and bass, as one Windows application. Not a plugin - a standalone `.exe`
with its own audio I/O (via [RtAudio](https://github.com/thestk/rtaudio), ASIO or WASAPI) and UI
(via [Dear ImGui](https://github.com/ocornut/imgui)), built on the NAM DSP in this repository.

This file is the reference: how to build it, how to set the audio up, and what the optional pieces
need. [The repository README](../README.md) says what the app is.

## What it does

- **A chain of blocks.** Up to twenty-four, each a NAM capture, a cab impulse response, a low/high
  cut, a compressor or a five-band parametric EQ. Drag them along the line to reorder.
- **Parallel routing.** Drag a block onto the lower lane to split the signal; the branches rejoin
  at the merge, where each has its own level. A split sends either the full signal to both branches
  or a crossover at a frequency you choose. Up to twelve parallel sections in one chain, each
  branch taking any number of blocks.
- **Per-block EQ** - low, mid and high with a sweepable mid - placed before or after the block.
- **A noise gate** with the same pre/post choice. Detection always listens to the clean input, so
  the threshold keeps meaning something about how hard you played.
- **Saved rigs**, with the one you had open coming back when the app does.
- **A player.** Import a song, separate it into stems, and mix them as channels with level, mute,
  solo and their own EQ. Loop by dragging in the ruler, change speed and key independently, and
  save the loops worth coming back to.
- **The song's grid.** Tempo, beats and downbeats drawn against the waveform, with a click that
  follows them through meter changes. Chord names across the track, single notes, or a full
  polyphonic transcription - and any of them played back on a small synth.
- **A strobe tuner** that names the note itself or locks to a string you pick, across twelve keys
  of every tuning it knows, with adjustable concert pitch and optional muting while you tune.
- **A metronome**, both as its own view and as a channel in the player.
- **A capture library** that scans a folder you point it at, with favourites and recents.
- **TONE3000 search and download** built in, so captures land in your library without a browser.

## Prerequisites

- **CMake** 3.14 or newer, on `PATH`
- **Visual Studio 2022** with the "Desktop development with C++" workload
- Internet access the first time you configure. GLFW, Dear ImGui, RtAudio, Eigen and
  AudioDSPTools are fetched automatically by CMake `FetchContent` as pinned source archives with
  checked hashes. `git` is not required for this, and no dependency needs downloading by hand.

## Build

From the repository root:

```
cmake -B build -S .
cmake --build build --config Release --target nam_standalone
```

The executable lands at `build/standalone/Release/Woodshed.exe`. Everything is linked
statically, so it is a single file you can copy anywhere.

A plain `git clone` without `--recursive` is fine: when `Dependencies/eigen` and
`Dependencies/AudioDSPTools` are empty, the root `CMakeLists.txt` fetches them instead.

To leave the app out of a build of this repo entirely, configure with
`-DNAM_BUILD_STANDALONE_UI=OFF`.

## Running

1. Launch `Woodshed.exe` and open **Settings**.
2. Pick an audio API - ASIO if your interface has a driver for it, WASAPI otherwise - then an
   input and output device, a sample rate and a buffer size, and click **Open**.
3. Point the capture folder at a directory of `.nam` and `.wav` files (this repo's
   `example_models/` will do to start).
4. Click a block on the chain line to select it, then load a capture or IR into it from the
   library.

Settings, the chain and the routing are remembered between runs in
`%APPDATA%\Woodshed\config.json`.

### TONE3000

The library view has a TONE3000 tab that searches the whole public library and downloads straight
into your capture folder.

Signing in uses OAuth 2.0 with PKCE: the app opens your browser, you sign in to TONE3000 there, and
it hands back a token. The app never sees your password, the token expires and is refreshed on its
own, and you can revoke it from your TONE3000 account at any time. The refresh token is encrypted
with the Windows data protection API (DPAPI) under your user account before it is written to the
config file, so it is never stored in plaintext and is not readable by another user on the machine.

Using it is one button: **Settings -> Sign in to TONE3000**. Nobody has to know what a token is.

### Building with a TONE3000 application key

That one button only works if the build carries a publishable key. It is the OAuth `client_id`: it
identifies *the application*, not the person using it, so one key serves every user of the build -
each of them signs in to their own TONE3000 account. TONE3000 documents the publishable key as safe
for client-side use; it authorises nothing on its own, and an authorization code is worthless
without the PKCE verifier that only the running app holds.

Create one under **Settings -> API Keys** on tone3000.com - it is the key labelled *Publishable
Key*, prefix `t3k_pub_`, not the secret key (`t3k_cs_`), which is a server-only credential and will
be rejected as an unknown `client_id` if you use it here by mistake. Then either paste it into
[`Tone3000AppKey.h`](Tone3000AppKey.h) or pass it at configure time to keep it out of the source:

```
cmake -B build -S . -DNAM_TONE3000_CLIENT_ID=t3k_pub_your_key
```

Nothing has to be registered for the redirect: TONE3000 accepts any redirect URI while a key has no
allowed list, and `http://localhost` and `http://127.0.0.1` are always accepted regardless. The app
listens on `http://localhost:8731/callback`, on the loopback interface and only while a sign-in is
in progress. If you do restrict a key to specific URIs, add that one to the list; Settings ->
Advanced in the app shows it with a Copy button, alongside the key it is currently sending.

A build with no key still runs; the TONE3000 tab says so, and Settings -> Advanced lets a user
supply their own.

### ASIO

RtAudio bundles the Steinberg ASIO host-side wrapper it compiles against, so there is no SDK to
download for the build. What you need at *runtime* is an ASIO **driver** for your interface -
most interfaces ship one, and [ASIO4ALL](https://www.asio4all.org) is the usual fallback if yours
does not.

If the ASIO device list comes up empty, the driver is not registered or not bound to the device.
Build the `asio_probe` target and run it: it talks to the ASIO wrapper directly and prints the
driver's own error message, which RtAudio otherwise discards in favour of a generic one.

```
cmake --build build --config Release --target asio_probe
build\standalone\Release\asio_probe.exe
```

## Limitations

Windows only - the audio backend, the file dialogs, the HTTP client and the key storage are all
platform APIs. Mono in, stereo out (the same signal on both), which is how amp captures work.
Twenty-four blocks and twelve parallel sections per chain; both are fixed so the audio thread's
working buffers can be allocated once, up front, and never resized while it is running.

Separating stems, transcribing every note and the deep beat tracker each drive a Python program as
a child process, so they need Python and that package installed. Everything else - the rig, the
player, the tuner, the metronome, chord names and single-note detection - runs in the app itself
and needs nothing.

