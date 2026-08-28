# TFRS Arma 3 extension (Task Force Radio Standalone)
#
# Build (from this directory):
#   cmake -S . -B build/x64   -A x64   && cmake --build build/x64   --config Release
#   cmake -S . -B build/win32 -A Win32 && cmake --build build/win32 --config Release
#
# Output file names follow what Arma 3 looks for:
#   64 bit client -> task_force_radio_pipe_x64.dll
#   32 bit client -> task_force_radio_pipe.dll

cmake_minimum_required(VERSION 3.15)

project(task_force_radio_pipe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "The TFRS extension is Windows only (Arma 3 + Win32 named pipes).")
endif()

# Statically link the CRT so users do not need the VC++ redistributable
# installed for Arma to be able to load the DLL.
# Requires CMP0091 = NEW, which cmake_minimum_required(3.15) already sets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TFRS_SOURCES
    src/CommandProcessor.cpp
    src/CommandProcessor.h
    src/Extension.cpp
    src/Json.cpp
    src/Json.h
    src/PipeClient.cpp
    src/PipeClient.h
    src/State.cpp
    src/State.h
    src/Util.cpp
    src/Util.h
    src/Version.h
)

add_library(task_force_radio_pipe SHARED ${TFRS_SOURCES})

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(task_force_radio_pipe PROPERTIES OUTPUT_NAME "task_force_radio_pipe_x64")
else()
    set_target_properties(task_force_radio_pipe PROPERTIES OUTPUT_NAME "task_force_radio_pipe")
endif()

set_target_properties(task_force_radio_pipe PROPERTIES PREFIX "")

target_include_directories(task_force_radio_pipe PRIVATE src)

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
