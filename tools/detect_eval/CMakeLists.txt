# Console tool: streams a WAV through NoteDetector and scores it against a MIDI/CSV ground truth.
add_executable(fall_detect_eval main.cpp)
target_link_libraries(fall_detect_eval PRIVATE fall_core fall_detect
    juce::juce_audio_formats
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
option(FALL_DETECT_TRACE "Print per-stage scores from NoteDetector to stderr" OFF)
if(FALL_DETECT_TRACE)
    target_compile_definitions(fall_detect_eval PRIVATE FALL_DETECT_TRACE=1)
endif()
