file(GLOB FALL_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(fall_tests ${FALL_TEST_SOURCES})
target_link_libraries(fall_tests PRIVATE fall_core fall_detect Catch2::Catch2WithMain
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_definitions(fall_tests PRIVATE
    FALL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(fall_tests)
if(FALL_DETECT_TRACE)
    target_compile_definitions(fall_tests PRIVATE FALL_DETECT_TRACE=1)
endif()
