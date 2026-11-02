cmake_minimum_required(VERSION 3.22)
project(Fall VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FALL_BUILD_APP       "Build the Fall desktop app"                  ON)
option(FALL_BUILD_TESTS     "Build unit tests"                            ON)
option(FALL_BUILD_TESTSYNTH "Build the tiny VST3 synth used by CI tests"  OFF)
option(FALL_LTO             "Link-time optimisation for the app (slow links)" ON)

include(FetchContent)
# Tip: -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE to reuse a local checkout.
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)
include(cmake/PatchJuceWasapiRaw.cmake)

# FST: GPLv3 reverse-engineered VST2 headers, so Fall can host older .dll instruments
# (Steinberg no longer distributes the VST2 SDK). Header-only; we don't build its CMake project.
FetchContent_Declare(FST
    GIT_REPOSITORY https://git.iem.at/zmoelnig/FST.git
    GIT_TAG        647af068765b75867e3a28b4dd8991ab9ed47f7c
    SOURCE_SUBDIR  do-not-build)
FetchContent_MakeAvailable(FST)

# Shared JUCE flags for every target we build.
add_library(fall_juce_flags INTERFACE)
target_compile_definitions(fall_juce_flags INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)

add_subdirectory(src/core)
add_subdirectory(src/detect)

if(FALL_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(FALL_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(tools/detect_eval)
endif()

if(FALL_BUILD_TESTSYNTH)
    enable_testing()  # plugin_host test is registered from tools/testsynth
    add_subdirectory(tools/testsynth)
endif()
