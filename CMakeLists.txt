cmake_minimum_required(VERSION 3.15) # CMP0091, which is how the MSVC runtime below is chosen

# --- one file, no redistributable ---
#
# Link the C++ runtime into the binary instead of against MSVCP140.dll and friends. Without this the
# app runs on this machine and fails on a machine that has never had Visual Studio on it, with a
# dialog naming a DLL that means nothing to the person reading it. The cost is about half a megabyte.
#
# CMP0091 is what makes the variable below the single place this is decided; under the old behaviour
# the /MD flag is baked into CMAKE_CXX_FLAGS and every subproject has to be talked out of it
# separately.
cmake_policy(SET CMP0091 NEW)
# Set unguarded, and before project(): MSVC is not defined until the compiler has been probed, so an
# if(MSVC) around this is false exactly when it matters. Compilers that are not MSVC ignore it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime linkage")

file(READ "${CMAKE_CURRENT_LIST_DIR}/NAM/version.h" _nam_version_header)
foreach(_component MAJOR MINOR PATCH)
	string(REGEX MATCH
		"#define[ \t]+NEURAL_AMP_MODELER_DSP_VERSION_${_component}[ \t]+([0-9]+)"
		_nam_version_match "${_nam_version_header}"
	)
	if(NOT _nam_version_match)
		message(FATAL_ERROR "Could not parse NEURAL_AMP_MODELER_DSP_VERSION_${_component} from NAM/version.h")
	endif()
	set(_nam_version_${_component} "${CMAKE_MATCH_1}")
endforeach()

set(_nam_project_version "${_nam_version_MAJOR}.${_nam_version_MINOR}.${_nam_version_PATCH}")
project(NAM VERSION "${_nam_project_version}")

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Release, Debug, RelWithDebInfo, MinSizeRel)" FORCE)
	message(STATUS "No build type specified; defaulting to Debug")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use libc++ on macOS, system default on Linux
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

option(NAM_USE_LIBCXX_LINUX "Use libc++ on Linux (Clang + libc++-dev packages; tests SlimmableWavenet _LIBCPP_VERSION path)" OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	include_directories(SYSTEM /usr/local/include)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if (NAM_USE_LIBCXX_LINUX)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
		# Clang links libc++/libc++abi; do not pull in libstdc++fs (libstdc++).
	else()
		link_libraries(stdc++fs)
	endif()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
	message(FATAL_ERROR "Unrecognized Platform!")
endif()

set(NAM_DEPS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies")

if(EXISTS "${NAM_DEPS_PATH}/eigen/Eigen/Core")
	include_directories(SYSTEM "${NAM_DEPS_PATH}/eigen")
else()
	# Dependencies/eigen is a submodule that hasn't been checked out (e.g. the repo was
	# downloaded as a zip instead of git-cloned). Fetch Eigen directly instead of requiring git.
	message(STATUS "Dependencies/eigen is empty - fetching Eigen 3.4.0 via CMake FetchContent")
	include(FetchContent)
	FetchContent_Declare(nam_eigen
		URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip
		URL_HASH SHA256=eba3f3d414d2f8cba2919c78ec6daab08fc71ba2ba4ae502b7e5d4d99fc02cda
	)
	FetchContent_GetProperties(nam_eigen)
	if(NOT nam_eigen_POPULATED)
		FetchContent_Populate(nam_eigen)
	endif()
	include_directories(SYSTEM "${nam_eigen_SOURCE_DIR}")
endif()

if(EXISTS "${NAM_DEPS_PATH}/AudioDSPTools/dsp/wav.cpp")
	set(NAM_AUDIO_DSP_TOOLS_DIR "${NAM_DEPS_PATH}/AudioDSPTools")
else()
	# Same situation as Dependencies/eigen above: submodule not checked out, fetch it instead.
	message(STATUS "Dependencies/AudioDSPTools is empty - fetching AudioDSPTools v0.1.2 via CMake FetchContent")
	include(FetchContent)
	FetchContent_Declare(nam_audiodsptools
		URL https://github.com/sdatkinson/AudioDSPTools/archive/refs/tags/v0.1.2.zip
		URL_HASH SHA256=225eabbb226e7623c2c06d4dc5c6bb992390dc75db0be8da29ee9e941389f972
	)
	FetchContent_GetProperties(nam_audiodsptools)
	if(NOT nam_audiodsptools_POPULATED)
		FetchContent_Populate(nam_audiodsptools)
	endif()
	set(NAM_AUDIO_DSP_TOOLS_DIR "${nam_audiodsptools_SOURCE_DIR}")
endif()

# Build the specialized A2 fast-path WaveNet (A2-Full + A2-Lite). When ON,
# models whose config matches the A2 shape signature are routed to a hand-optimized
# WaveNet implementation instead of the generic one. When OFF, all models go
# through the generic path.
option(NAM_ENABLE_A2_FAST "Build the A2 fast-path WaveNet" ON)
if(NAM_ENABLE_A2_FAST)
	add_compile_definitions(NAM_ENABLE_A2_FAST)
endif()

add_subdirectory(tools)

option(NAM_BUILD_STANDALONE_UI "Build the standalone realtime capture-browser UI (nam_standalone)" ON)
if(NAM_BUILD_STANDALONE_UI)
	add_subdirectory(standalone)
endif()

#file(MAKE_DIRECTORY build/tools)

#add_custom_target(copy_tools ALL
#	${CMAKE_COMMAND} -E copy "$<TARGET_FILE:tools>" tools/
#	DEPENDS tools
#)
