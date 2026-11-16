#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>

// Sends a copy of the host's audio to a Windows (WASAPI shared mode) output device.
//
// The host's audio thread pushes samples into a lock-free FIFO. The output device's
// own thread pulls them out and resamples them. The two devices run on different
// clocks, so the resampling speed is nudged up or down a tiny bit to keep the FIFO
// near a fixed fill level.
class BridgeOutput : private juce::AudioIODeviceCallback
{
public:
    BridgeOutput();
    ~BridgeOutput() override;

    // Message thread only.
    juce::StringArray getOutputDeviceNames();
    juce::String openDevice (const juce::String& deviceName);   // returns an error, or empty on success
    void closeDevice();
    juce::String getOpenDeviceName() const     { return openDeviceName; }
    double getDeviceSampleRate() const         { return deviceSampleRate.load(); }

    // Host audio thread only.
    void setHostSampleRate (double newRate)    { hostSampleRate.store (newRate); }
    void setHostBlockSize (int newSize)        { hostBlockSize.store (newSize); }
    void setEnabled (bool shouldBeEnabled)     { enabled.store (shouldBeEnabled); }
    void setGain (float newGain)               { gain.store (newGain); }
    void push (const juce::AudioBuffer<float>& buffer);

    // Any thread.
    bool isRunning() const                     { return running.load(); }
    double getBufferedMs() const;
    int getDropoutCount() const                { return dropouts.load(); }
    juce::String getLastError() const;

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

    static constexpr int fifoSize = 1 << 15;
    static constexpr double targetLatencySeconds = 0.04;
    static constexpr double maxSpeedCorrection = 0.002;

    std::unique_ptr<juce::AudioIODeviceType> deviceType;
    std::unique_ptr<juce::AudioIODevice> device;
    juce::String openDeviceName;

    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> fifoBuffer { 2, fifoSize };

    // Used only by the output device thread.
    juce::AudioBuffer<float> scratch, rendered;
    std::array<juce::LagrangeInterpolator, 2> interpolators;
    juce::LinearSmoothedValue<float> smoothedGain;
    double smoothedFill = 0.0;
    bool priming = true;

    std::atomic<double> hostSampleRate { 48000.0 };
    std::atomic<double> deviceSampleRate { 0.0 };
    std::atomic<int> hostBlockSize { 512 };
    std::atomic<bool> enabled { true };
    std::atomic<bool> running { false };
    std::atomic<float> gain { 1.0f };
    std::atomic<int> dropouts { 0 };

    mutable juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BridgeOutput)
};
