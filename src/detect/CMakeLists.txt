# Real-time piano note detection from a microphone (no ML, plain DSP).
add_library(fall_detect INTERFACE)
file(GLOB FALL_DETECT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
target_sources(fall_detect INTERFACE ${FALL_DETECT_SOURCES})
target_include_directories(fall_detect INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fall_detect INTERFACE juce::juce_core juce::juce_audio_basics juce::juce_dsp fall_juce_flags)
