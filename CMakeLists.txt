cmake_minimum_required(VERSION 3.22)

project(STREAM_BRIDGE)

# The version comes from the newest git tag (like v0.3.0). A release build shows the tag,
# other builds show how far past it they are, like v0.3.0-2-gabc1234.
set(STREAM_BRIDGE_VERSION_TEXT "dev")
set(STREAM_BRIDGE_VERSION_NUMBER "0.0.0")
find_package(Git QUIET)

if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE gitDescribe
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE gitDescribeResult)

    if(gitDescribeResult EQUAL 0 AND gitDescribe MATCHES "^v([0-9]+[.][0-9]+[.][0-9]+)")
        set(STREAM_BRIDGE_VERSION_TEXT "${gitDescribe}")
        set(STREAM_BRIDGE_VERSION_NUMBER "${CMAKE_MATCH_1}")
    endif()
endif()

message(STATUS "Stream Bridge version: ${STREAM_BRIDGE_VERSION_TEXT}")

# Put the Visual C++ runtime inside the plugin, so it works on PCs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(libs/JUCE)

juce_add_plugin(StreamBridge
    VERSION ${STREAM_BRIDGE_VERSION_NUMBER}
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
        Source/AudioDownloader.cpp
        Source/BridgeOutput.cpp
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp
        Source/WindowsAudioCheck.cpp)

target_compile_definitions(StreamBridge
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        "STREAM_BRIDGE_VERSION=\"${STREAM_BRIDGE_VERSION_TEXT}\"")

target_link_libraries(StreamBridge
    PRIVATE
        juce::juce_audio_devices
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Test program: runs the plugin code like Ableton does and measures it.
juce_add_console_app(LatencyTest
    VERSION ${STREAM_BRIDGE_VERSION_NUMBER}
    PRODUCT_NAME "Latency Test")

target_sources(LatencyTest
    PRIVATE
        Source/AudioDownloader.cpp
        Source/BridgeOutput.cpp
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp
        Source/WindowsAudioCheck.cpp
        tests/LatencyTest.cpp)

target_compile_definitions(LatencyTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        "JucePlugin_Name=\"Stream Bridge\"")

target_link_libraries(LatencyTest
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_cryptography
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Test program: measures how close the copy for viewers is to a perfect copy.
juce_add_console_app(CopyQualityTest
    VERSION ${STREAM_BRIDGE_VERSION_NUMBER}
    PRODUCT_NAME "Copy Quality Test")

target_sources(CopyQualityTest
    PRIVATE
        Source/BridgeOutput.cpp
        tests/CopyQualityTest.cpp)

target_compile_definitions(CopyQualityTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(CopyQualityTest
    PRIVATE
        juce::juce_audio_devices
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Test program: runs the YouTube downloader without a DAW.
juce_add_console_app(DownloadTest
    VERSION ${STREAM_BRIDGE_VERSION_NUMBER}
    PRODUCT_NAME "Download Test")

target_sources(DownloadTest
    PRIVATE
        Source/AudioDownloader.cpp
        tests/DownloadTest.cpp)

target_compile_definitions(DownloadTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(DownloadTest
    PRIVATE
        juce::juce_core
        juce::juce_cryptography
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# The small ffmpeg for the YouTube downloader, made by ffmpeg/build.sh (build.bat runs it).
# It goes into Stream Bridge.vst3/Contents/Resources, and next to DownloadTest.exe.
set(STREAM_BRIDGE_FFMPEG_DIR "${CMAKE_BINARY_DIR}/ffmpeg" CACHE PATH "Folder with the ffmpeg.exe made by ffmpeg/build.sh")

if(EXISTS "${STREAM_BRIDGE_FFMPEG_DIR}/ffmpeg.exe")
    set(ffmpegFiles
        "${STREAM_BRIDGE_FFMPEG_DIR}/ffmpeg.exe"
        "${STREAM_BRIDGE_FFMPEG_DIR}/ffmpeg-LICENSE.txt"
        "${STREAM_BRIDGE_FFMPEG_DIR}/ffmpeg-README.txt")

    add_custom_command(TARGET StreamBridge_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:StreamBridge_VST3>/../Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ffmpegFiles} "$<TARGET_FILE_DIR:StreamBridge_VST3>/../Resources")

    add_custom_command(TARGET DownloadTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STREAM_BRIDGE_FFMPEG_DIR}/ffmpeg.exe" "$<TARGET_FILE_DIR:DownloadTest>")
else()
    message(WARNING "ffmpeg.exe not found in ${STREAM_BRIDGE_FFMPEG_DIR}. The plugin will build, "
                    "but its YouTube downloader cannot convert. Run ffmpeg/build.sh in MSYS2 first.")
endif()
