#include "WindowsAudioCheck.h"

#include <iterator>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment (lib, "advapi32.lib")

namespace
{
    // Windows keeps each sound device's settings here, one key per device.
    const wchar_t* const recordingDevicesKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
    const wchar_t* const deviceNameValue     = L"{a45c254e-df1c-4efd-8020-67d146a850e0},2";   // e.g. "CABLE Output"
    const wchar_t* const soundCardNameValue  = L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6";   // e.g. "VB-Audio Virtual Cable"
    const wchar_t* const listenValue         = L"{24dbb0fc-9311-4b3d-9cf0-18ff155639d4},1";   // "Listen to this device"
    constexpr DWORD deviceStateActive = 1;
    constexpr unsigned char variantTypeBool = 11;   // VT_BOOL

    juce::String readString (HKEY key, const juce::String& subKey, const wchar_t* name)
    {
        wchar_t buffer[512] {};
        DWORD size = sizeof (buffer);

        if (RegGetValueW (key, subKey.toWideCharPointer(), name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
            return {};

        return juce::String (buffer);
    }
}

bool WindowsAudioCheck::looksVirtual (const juce::String& outputDeviceName)
{
    for (const auto* keyword : { "CABLE", "Virtual", "VB-Audio", "Voicemeeter", "Wave Link", "Steam Streaming" })
        if (outputDeviceName.containsIgnoreCase (keyword))
            return true;

    return false;
}

juce::String WindowsAudioCheck::findListeningRecordingDevice (const juce::String& outputDeviceName)
{
    // Windows names outputs "<device> (<sound card>)".
    const auto soundCard = outputDeviceName.fromLastOccurrenceOf ("(", false, false)
                                           .upToLastOccurrenceOf (")", false, false).trim();

    if (soundCard.isEmpty())
        return {};

    HKEY devices = nullptr;

    if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, recordingDevicesKey, 0, KEY_READ | KEY_WOW64_64KEY, &devices) != ERROR_SUCCESS)
        return {};

    const juce::ScopeGuard closeKey { [&] { RegCloseKey (devices); } };

    for (DWORD i = 0;; ++i)
    {
        wchar_t id[256] {};
        DWORD idLength = (DWORD) std::size (id);

        if (RegEnumKeyExW (devices, i, id, &idLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        DWORD state = 0;
        DWORD size = sizeof (state);

        if (RegGetValueW (devices, id, L"DeviceState", RRF_RT_REG_DWORD, nullptr, &state, &size) != ERROR_SUCCESS
             || state != deviceStateActive)
            continue;

        const auto properties = juce::String (id) + "\\Properties";

        if (! readString (devices, properties, soundCardNameValue).equalsIgnoreCase (soundCard))
            continue;

        unsigned char listen[32] {};
        size = sizeof (listen);

        if (RegGetValueW (devices, properties.toWideCharPointer(), listenValue, RRF_RT_REG_BINARY, nullptr, listen, &size) != ERROR_SUCCESS)
            continue;

        // The value is a PROPVARIANT: type VT_BOOL (11) first, the value at byte 8. True is 0xFFFF.
        if (size >= 10 && listen[0] == variantTypeBool && (listen[8] != 0 || listen[9] != 0))
            return readString (devices, properties, deviceNameValue);
    }

    return {};
}
