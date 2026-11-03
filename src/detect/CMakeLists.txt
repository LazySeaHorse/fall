# Real-time piano note detection from a microphone: loudness gate + aubio pitch tracker.
add_library(fall_detect INTERFACE)
target_sources(fall_detect INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/NoteDetector.cpp)
target_include_directories(fall_detect INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fall_detect INTERFACE juce::juce_core juce::juce_audio_basics fall_aubio fall_juce_flags)
