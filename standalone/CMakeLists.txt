cmake_minimum_required(VERSION 3.14)

include(FetchContent)

# Build all fetched dependencies (and this target) as static libs so the result is a single
# self-contained nam_standalone.exe with no extra DLLs to ship alongside it.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- GLFW (window + OpenGL context creation) ---
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
	URL https://github.com/glfw/glfw/archive/refs/tags/3.5.1.zip
	URL_HASH SHA256=e9a80355e8a0c59b15ae8576c2c3aeae792c2b1082ec426dc93bde70d5017fda
)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui ---
# ImGui's upstream repo ships no CMakeLists.txt, so only fetch/populate the source and build
# our own static library target from the files we need (core + GLFW/OpenGL3 backends).
FetchContent_Declare(imgui
	URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
	URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
	FetchContent_Populate(imgui)
endif()

add_library(nam_ui_imgui STATIC
	"${imgui_SOURCE_DIR}/imgui.cpp"
	"${imgui_SOURCE_DIR}/imgui_draw.cpp"
	"${imgui_SOURCE_DIR}/imgui_tables.cpp"
	"${imgui_SOURCE_DIR}/imgui_widgets.cpp"
	"${imgui_SOURCE_DIR}/imgui_demo.cpp"
	"${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
	"${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(nam_ui_imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(nam_ui_imgui PUBLIC glfw)

# --- RtAudio (realtime audio I/O) ---
# ASIO support: RtAudio's own "include/" directory already bundles the Steinberg ASIO host
# wrapper files it needs (asio.cpp, iasiodrv.h, ...), so no separate SDK download is required.
# At runtime, ASIO still needs a vendor driver installed for the audio interface in use
# (or a generic option such as ASIO4ALL) - that's a driver, not the SDK, and is unrelated to
# this build step.
set(RTAUDIO_API_WASAPI ON CACHE BOOL "" FORCE)
set(RTAUDIO_API_ASIO ON CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Keep the MSVC runtime linkage consistent with the rest of this build (glfw/imgui/our own
# sources all use the default dynamic /MD runtime); RtAudio's own CMakeLists otherwise
# switches to /MT when built static, which would cause a runtime-library mismatch at link time.
set(RTAUDIO_STATIC_MSVCRT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rtaudio
	URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.zip
	URL_HASH SHA256=ac27dfbfa764441b2b14a7180861d044d944189b5d792cb231fbffc540220fc5
)
FetchContent_MakeAvailable(rtaudio)

if(MSVC)
	# RtAudio's ASIO wrapper (include/asio.cpp, asiodrivers.cpp, asiolist.cpp) needs full COM
	# support (CoInitialize, IASIO, CLSCTX_INPROC_SERVER, ...) from <windows.h>. The root
	# CMakeLists.txt defines WIN32_LEAN_AND_MEAN globally on Windows, which strips that out and
	# is inherited here since this target is created inside this subdirectory's scope. Undefine
	# it just for this one target rather than touching the definition everywhere else uses it.
	target_compile_options(rtaudio PRIVATE /UWIN32_LEAN_AND_MEAN)
endif()

# --- NAM core sources (same glob tools/CMakeLists.txt uses) ---
file(GLOB NAM_SOURCES_TOP "${CMAKE_CURRENT_SOURCE_DIR}/../NAM/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../NAM/*.c")
file(GLOB NAM_SOURCES_SUB "${CMAKE_CURRENT_SOURCE_DIR}/../NAM/*/*.cpp")
set(NAM_SOURCES ${NAM_SOURCES_TOP} ${NAM_SOURCES_SUB})

find_package(OpenGL REQUIRED)

add_executable(nam_standalone
	main.cpp
	AudioEngine.cpp
	AudioFile.cpp
	CaptureLibrary.cpp
	Compressor.cpp
	Config.cpp
	DeepBeatTracker.cpp
	HttpClient.cpp
	Metronome.cpp
	NoteAnalysis.cpp
	NoteSynth.cpp
	OAuth.cpp
	ParametricEq.cpp
	Player.cpp
	PolyphonicTranscriber.cpp
	Project.cpp
	SecretStore.cpp
	StageEq.cpp
	StemSeparator.cpp
	Subprocess.cpp
	TempoAnalysis.cpp
	Tone3000Client.cpp
	Tuner.cpp
	# The tone stack's biquads and the cab IR loader: the same AudioDSPTools modules the official
	# NAM plugin uses. ImpulseResponse pulls in wav.cpp for loading and Resample.h for rate
	# conversion (header-only).
	"${NAM_AUDIO_DSP_TOOLS_DIR}/dsp/dsp.cpp"
	"${NAM_AUDIO_DSP_TOOLS_DIR}/dsp/RecursiveLinearFilter.cpp"
	"${NAM_AUDIO_DSP_TOOLS_DIR}/dsp/ImpulseResponse.cpp"
	"${NAM_AUDIO_DSP_TOOLS_DIR}/dsp/NoiseGate.cpp"
	"${NAM_AUDIO_DSP_TOOLS_DIR}/dsp/wav.cpp"
	Win32FileDialog.cpp
	WindowChrome.cpp
	ui/MainWindow.cpp
	ui/Theme.cpp
	${NAM_SOURCES}
	# The executable's icon. A resource, not a runtime call: Explorer, the taskbar and the Alt-Tab
	# list read it from the file itself, long before any of our code runs.
	$<$<BOOL:${WIN32}>:${CMAKE_CURRENT_SOURCE_DIR}/woodshed.rc>
)

target_include_directories(nam_standalone PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
	"${NAM_DEPS_PATH}/nlohmann"
	"${NAM_DEPS_PATH}/dr_libs"
	"${NAM_DEPS_PATH}/signalsmith"
	"${NAM_AUDIO_DSP_TOOLS_DIR}/dsp"
)

# NAM_SAMPLE_FLOAT keeps NAM_SAMPLE == float end-to-end, matching the RTAUDIO_FLOAT32 stream
# format so the audio callback needs no per-sample float<->double conversion.
# NAM runs in float (matching the RTAUDIO_FLOAT32 stream) while AudioDSPTools stays on its default
# double: ImpulseResponse::Process is hard-coded to double** even though its base class declares
# DSP_SAMPLE**, so defining DSP_SAMPLE_FLOAT here would not compile. The engine converts at the
# two boundaries instead, which costs nothing measurable at these buffer sizes.
target_compile_definitions(nam_standalone PRIVATE NAM_SAMPLE_FLOAT NOMINMAX WIN32_LEAN_AND_MEAN)

# The app's TONE3000 publishable key (its OAuth client_id). Identifies the application, not the
# user, and is documented by TONE3000 as safe for client-side use - so it belongs in the binary.
# Set it here to keep it out of the source tree; see standalone/Tone3000AppKey.h.
set(NAM_TONE3000_CLIENT_ID "" CACHE STRING "TONE3000 publishable key to build into the app")
if(NAM_TONE3000_CLIENT_ID)
	target_compile_definitions(nam_standalone PRIVATE NAM_TONE3000_CLIENT_ID="${NAM_TONE3000_CLIENT_ID}")
endif()
target_compile_features(nam_standalone PRIVATE cxx_std_20)
if(MSVC)
	# Treat sources as UTF-8 so any non-ASCII in UI strings reaches ImGui intact rather than
	# being reinterpreted through the system ANSI codepage.
	target_compile_options(nam_standalone PRIVATE /utf-8)
endif()
set_target_properties(nam_standalone PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)

# winhttp: TONE3000 HTTPS calls. crypt32: DPAPI encryption of the stored refresh token.
# bcrypt: the PKCE random bytes and SHA-256. ws2_32: the loopback listener the OAuth redirect
# comes back to. dwmapi: colouring the title bar in the app's own black.
target_link_libraries(nam_standalone PRIVATE nam_ui_imgui glfw rtaudio OpenGL::GL ole32 shell32 winhttp crypt32
	bcrypt ws2_32 dwmapi)

# --- asio_probe: console diagnostic -------------------------------------------------------
# Talks to the ASIO host wrapper directly (NOT through RtAudio) so it can print the driver's own
# ASIODriverInfo::errorMessage, which RtAudio throws away in favour of a generic ASE_NotPresent
# string. Compiles the wrapper sources itself rather than linking rtaudio, to avoid duplicate
# definitions of the ASIO globals (theAsioDriver et al.) that live in asio.cpp.
if(WIN32)
	add_executable(asio_probe
		asio_probe.cpp
		"${rtaudio_SOURCE_DIR}/include/asio.cpp"
		"${rtaudio_SOURCE_DIR}/include/asiodrivers.cpp"
		"${rtaudio_SOURCE_DIR}/include/asiolist.cpp"
		"${rtaudio_SOURCE_DIR}/include/iasiothiscallresolver.cpp"
	)
	target_include_directories(asio_probe PRIVATE "${rtaudio_SOURCE_DIR}/include")
	# Same reason as the rtaudio target above: the ASIO wrapper needs the full <windows.h>.
	if(MSVC)
		target_compile_options(asio_probe PRIVATE /UWIN32_LEAN_AND_MEAN)
	endif()
	target_link_libraries(asio_probe PRIVATE ole32 winmm)
endif()
