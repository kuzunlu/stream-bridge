#pragma once

#include <juce_core/juce_core.h>

// Checks for Windows audio settings that make the user hear the copy themselves.
// Message thread only. Each check takes well under a millisecond, and they run only
// when an output is opened or the plugin window is opened.
namespace WindowsAudioCheck
{
    // True for outputs that nobody listens to directly, like VB-Audio's virtual cables.
    bool looksVirtual (const juce::String& outputDeviceName);

    // The name of a recording device on the same sound card as the output (for example
    // "CABLE Output" for "CABLE Input (VB-Audio Virtual Cable)") that has Windows'
    // "Listen to this device" switched on, or empty if there is none.
    juce::String findListeningRecordingDevice (const juce::String& outputDeviceName);
}
