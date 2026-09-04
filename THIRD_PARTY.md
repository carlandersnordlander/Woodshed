# Third-party components

Woodshed is the application under [`standalone/`](standalone/). It is built on
[NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore), whose sources are
carried in this repository rather than fetched; its MIT licence and copyright notice are kept in
[`LICENSE`](LICENSE) and cover the DSP core.

## Bundled in this repository

Carried here as source. The three single-header libraries are vendored rather than fetched at
configure time because none of them has tagged releases, so there is no URL with a hash that stays
valid. Each folder has an `info.txt` saying where its contents came from and which files were taken.

| Component | Where | Licence |
| --- | --- | --- |
| NAM core DSP | `NAM/`, `tools/` | MIT - Copyright (c) 2023 Steven Atkinson, see `LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) | `Dependencies/nlohmann/json.hpp` | MIT - notice at the top of the header |
| [dr_libs](https://github.com/mackron/dr_libs) | `Dependencies/dr_libs/` | Public domain or MIT-0, at your choice - statements at the foot of each header |
| [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | `Dependencies/signalsmith/signalsmith-stretch.h` | MIT |
| [signalsmith-linear](https://github.com/Signalsmith-Audio/linear) | `Dependencies/signalsmith/signalsmith-linear/` | MIT |

## Fetched at configure time

None of these are redistributed here. CMake downloads each one as a pinned source archive with a
verified SHA256, so a checkout of this repository contains no third-party binaries.

| Component | Version | Licence |
| --- | --- | --- |
| [Eigen](https://gitlab.com/libeigen/eigen) | 3.4.0 | MPL 2.0 |
| [AudioDSPTools](https://github.com/sdatkinson/AudioDSPTools) | 0.1.2 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | MIT |
| [GLFW](https://github.com/glfw/glfw) | 3.5.1 | zlib/libpng |
| [RtAudio](https://github.com/thestk/rtaudio) | 6.0.1 | MIT-style, see its `LICENSE` |

Eigen and AudioDSPTools are only fetched when `Dependencies/eigen` and
`Dependencies/AudioDSPTools` have no sources in them, which is what a clone of this repository
gives you - upstream carries them as git submodules, and here they are fetched by CMake instead, so
that building needs neither `git` nor a recursive clone. Put sources in either folder and the local
copy is used.

## Run as separate programs, if installed

Nothing here is bundled, fetched or linked. These are Python programs the app starts as child
processes when you ask for the feature that needs one, and the app works without them - it says so
where the feature would be. Installing them is the person's own choice, under each project's own
licence.

| Component | What it does | Licence |
| --- | --- | --- |
| [Demucs](https://github.com/adefossez/demucs) | separates a song into stems | MIT |
| [basic-pitch](https://github.com/spotify/basic-pitch) | transcribes every note of every voice | Apache 2.0 |
| [Beat This!](https://github.com/CPJKU/beat_this) | finds the beats and downbeats | MIT |

## A note on ASIO

The standalone app builds with ASIO support enabled. The host-side ASIO wrapper it compiles
against is Steinberg's, and ships inside RtAudio's own repository - so it arrives with RtAudio at
configure time and is not redistributed here either.

That is settled for building from source. **Distributing compiled binaries** with ASIO enabled is
a separate question: Steinberg's ASIO SDK licence has terms about that, and anyone shipping a
build of this app should read them rather than assume. Building it yourself and running it on
your own machine, which is what this repository is for, is not affected.
