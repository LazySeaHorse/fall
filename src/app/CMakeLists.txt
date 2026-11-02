juce_add_gui_app(Fall
    PRODUCT_NAME "Fall"
    COMPANY_NAME "Fall"
    BUNDLE_ID "app.fall.piano"
    ICON_BIG "${CMAKE_SOURCE_DIR}/assets/icon_1024.png"
    VERSION ${PROJECT_VERSION})

file(GLOB FALL_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
target_sources(Fall PRIVATE ${FALL_APP_SOURCES})

target_compile_definitions(Fall PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Fall,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Fall,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_ASIO=0
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_JACK=0)

target_include_directories(Fall PRIVATE ${CMAKE_SOURCE_DIR}/cmake/vst2shim ${fst_SOURCE_DIR})

target_link_libraries(Fall PRIVATE
    fall_core
    fall_detect
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(FALL_LTO)
    target_link_libraries(Fall PRIVATE juce::juce_recommended_lto_flags)
endif()
