cmake_minimum_required(VERSION 3.22)

# Universal (Apple Silicon + Intel) builds on macOS. Must be set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")

project(BeatJacker VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use a local JUCE checkout with -DJUCE_PATH=/path/to/JUCE, otherwise it is downloaded.
if(JUCE_PATH)
    add_subdirectory(${JUCE_PATH} juce)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(BeatJacker
    PRODUCT_NAME "Beat Jacker"
    COMPANY_NAME "Dev Griffin"
    BUNDLE_ID "com.devgriffin.beatjacker"
    PLUGIN_MANUFACTURER_CODE Dvgr
    PLUGIN_CODE Bjkr
    FORMATS VST3 AU Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Drum
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(BeatJacker PRIVATE
    Source/Engine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(BeatJacker PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    _USE_MATH_DEFINES=1)

target_link_libraries(BeatJacker
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

# Engine self-test (no JUCE needed): cmake --build build --target engine_test
add_executable(engine_test EXCLUDE_FROM_ALL tools/engine_test.cpp Source/Engine.cpp)
target_compile_definitions(engine_test PRIVATE _USE_MATH_DEFINES=1)
