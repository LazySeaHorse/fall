# Pure engine logic: song model, MIDI import, playback/learn-mode state machine.
# INTERFACE so each consumer compiles the sources alongside its own JUCE modules.
add_library(fall_core INTERFACE)
target_sources(fall_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Song.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Player.cpp)
target_include_directories(fall_core INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fall_core INTERFACE juce::juce_core juce::juce_audio_basics fall_juce_flags)
