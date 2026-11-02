# FallTestSynth: a tiny polyphonic sine VST3 instrument used only by CI to
# exercise plugin hosting. Enabled with -DFALL_BUILD_TESTSYNTH=ON.

juce_add_plugin(FallTestSynth
    PRODUCT_NAME "FallTestSynth"
    COMPANY_NAME "Fall"
    BUNDLE_ID "app.fall.testsynth"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Fall
    PLUGIN_CODE Fts1
    FORMATS VST3
    VST3_CATEGORIES Instrument Synth
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(FallTestSynth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/TestSynth.cpp)

target_compile_definitions(FallTestSynth PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(FallTestSynth
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    PUBLIC
        juce::juce_audio_processors
        juce::juce_audio_plugin_client)

# Host-side test that loads the plugin built above.
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/plugin_host ${CMAKE_BINARY_DIR}/tests/plugin_host)
