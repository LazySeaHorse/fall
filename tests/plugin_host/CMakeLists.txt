# Host-side plugin test. Added from tools/testsynth/CMakeLists.txt, so it only
# exists when -DFALL_BUILD_TESTSYNTH=ON.

add_executable(fall_plugin_host_test PluginHostTest.cpp)

target_compile_definitions(fall_plugin_host_test PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_AU=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(fall_plugin_host_test PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Make sure the plugin is built before the test runs.
add_dependencies(fall_plugin_host_test FallTestSynth_VST3)

# The bundle path is resolved at generate time from the plugin target, so it is
# right for single- and multi-config generators.
add_test(NAME plugin_host
    COMMAND fall_plugin_host_test "$<GENEX_EVAL:$<TARGET_PROPERTY:FallTestSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
set_tests_properties(plugin_host PROPERTIES TIMEOUT 60 LABELS "plugin")
