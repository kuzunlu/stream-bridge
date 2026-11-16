cmake_minimum_required(VERSION 3.22)

project(STREAM_BRIDGE VERSION 0.1.0)

# Put the Visual C++ runtime inside the plugin, so it works on PCs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(libs/JUCE)

juce_add_plugin(StreamBridge
    COMPANY_NAME "Kursat"
    PLUGIN_MANUFACTURER_CODE Kurs
    PLUGIN_CODE Stbr
    FORMATS VST3
    PRODUCT_NAME "Stream Bridge"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(StreamBridge
    PRIVATE
        Source/BridgeOutput.cpp
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp)

target_compile_definitions(StreamBridge
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(StreamBridge
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Test program: runs the plugin code like Ableton does and measures it.
juce_add_console_app(LatencyTest
    PRODUCT_NAME "Latency Test")

target_sources(LatencyTest
    PRIVATE
        Source/BridgeOutput.cpp
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp
        tests/LatencyTest.cpp)

target_compile_definitions(LatencyTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        "JucePlugin_Name=\"Stream Bridge\"")

target_link_libraries(LatencyTest
    PRIVATE
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
