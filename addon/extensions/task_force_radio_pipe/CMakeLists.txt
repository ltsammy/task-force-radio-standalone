# TFRS Arma 3 extension (Task Force Radio Standalone)
#
# Build (from this directory):
#   cmake -S . -B build/x64   -A x64   && cmake --build build/x64   --config Release
#   cmake -S . -B build/win32 -A Win32 && cmake --build build/win32 --config Release
#
# Output file names follow what Arma 3 looks for:
#   64 bit client -> task_force_radio_pipe_x64.dll
#   32 bit client -> task_force_radio_pipe.dll

cmake_minimum_required(VERSION 3.16)  # required by the vendored libopus (third_party/opus)

project(task_force_radio_pipe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "The TFRS extension is Windows only (Arma 3 + Win32 named pipes).")
endif()

# Statically link the CRT so users do not need the VC++ redistributable
# installed for Arma to be able to load the DLL.
# Requires CMP0091 = NEW, which cmake_minimum_required(3.16) already sets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Vendored libopus (third_party/opus, pinned to upstream v1.6.1) -- the one genuine third-party
# dependency this extension carries, needed for the native voice port (see the port's plan doc).
# Force every knob to match our static-CRT/no-extras build *before* add_subdirectory: none of this
# needs to be user-configurable, and forcing it here means a stray cached value from some other
# opus consumer on the same machine can never silently change what we link.
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_FUZZING OFF CACHE BOOL "" FORCE)
# These three pull in the dnn/lpcnet ML bandwidth-extension code (~15 MB of source, not vendored
# here at all -- see third_party/opus's pruned layout) for features this project doesn't use
# (classic 48kHz mono VOIP only).
set(OPUS_DEEP_PLC OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
# Defensive: re-assert the static CRT on the opus target directly, regardless of whether
# OPUS_STATIC_RUNTIME actually wired it up internally.
set_target_properties(opus PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TFRS_SOURCES
    src/CommandProcessor.cpp
    src/CommandProcessor.h
    src/Extension.cpp
    src/State.cpp
    src/State.h
    src/Util.cpp
    src/Util.h
    src/Version.h
    src/Voice/AudioDeviceUtil.cpp
    src/Voice/AudioDeviceUtil.h
    src/Voice/Dsp/BiquadFilter.cpp
    src/Voice/Dsp/BiquadFilter.h
    src/Voice/Dsp/Panning.h
    src/Voice/Dsp/RadioEffectChain.cpp
    src/Voice/Dsp/RadioEffectChain.h
    src/Voice/Log.cpp
    src/Voice/Log.h
    src/Voice/OpusCodec.cpp
    src/Voice/OpusCodec.h
    src/Voice/PlaybackMixer.cpp
    src/Voice/PlaybackMixer.h
    src/Voice/Protocol.cpp
    src/Voice/Protocol.h
    src/Voice/RemoteVoiceSource.cpp
    src/Voice/RemoteVoiceSource.h
    src/Voice/TransmitController.cpp
    src/Voice/TransmitController.h
    src/Voice/VoiceNetworkClient.cpp
    src/Voice/VoiceNetworkClient.h
    src/Voice/VoiceSession.cpp
    src/Voice/VoiceSession.h
    src/Voice/WasapiCaptureEngine.cpp
    src/Voice/WasapiCaptureEngine.h
)

add_library(task_force_radio_pipe SHARED ${TFRS_SOURCES})

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(task_force_radio_pipe PROPERTIES OUTPUT_NAME "task_force_radio_pipe_x64")
else()
    set_target_properties(task_force_radio_pipe PROPERTIES OUTPUT_NAME "task_force_radio_pipe")
endif()

set_target_properties(task_force_radio_pipe PROPERTIES PREFIX "")

target_include_directories(task_force_radio_pipe PRIVATE src)
target_link_libraries(task_force_radio_pipe PRIVATE
    opus
    Ws2_32    # VoiceNetworkClient's UDP socket (Winsock2)
    Ole32     # WASAPI device/COM activation (AudioDeviceUtil, WasapiCaptureEngine, PlaybackMixer)
    Bcrypt    # SHA-256 password hash for the ConnectRequest handshake (CNG, not a vendored dep)
)

target_compile_definitions(task_force_radio_pipe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601   # Windows 7 and up
)

if(MSVC)
    target_compile_options(task_force_radio_pipe PRIVATE /W4 /EHsc /permissive-)
    # /Zc:__cplusplus keeps __cplusplus honest, /utf-8 makes the sources
    # portable across code pages (the protocol uses raw 0x0A/0x0B/0x10 bytes).
    target_compile_options(task_force_radio_pipe PRIVATE /Zc:__cplusplus /utf-8)
    target_compile_options(task_force_radio_pipe PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

install(TARGETS task_force_radio_pipe RUNTIME DESTINATION .)
