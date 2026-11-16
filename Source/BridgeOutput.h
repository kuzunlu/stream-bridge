#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <climits>
#include <optional>

// Sends a copy of the host's audio to a Windows (WASAPI shared mode) output device.
//
// The host's audio thread pushes samples into a lock-free FIFO. The output device's
// own thread pulls them out and resamples them. The two devices run on different
// clocks, so the resampling speed is nudged up or down a tiny bit to keep the FIFO
// near a fixed fill level.
class BridgeOutput : private juce::AudioIODeviceCallback,
                     private juce::AudioIODeviceType::Listener
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
    bool couldUseHostSampleRate() const;       // running, but reopening would avoid resampling

    // Called on the message thread when the device stopped by itself (for example after
    // Windows sleep or a driver reset) or the list of devices changed while not running,
    // so the owner can open the device again. Costs nothing while everything works.
    std::function<void()> onReconnectNeeded;

    // Host audio thread only.
    void setHostSampleRate (double newRate)    { hostSampleRate.store (newRate); }
    void setHostBlockSize (int newSize)        { hostBlockSize.store (newSize); }
    void setTargetLatencyMs (float newMs)      { targetLatencyMs.store (newMs); }
    void setEnabled (bool shouldBeEnabled)     { enabled.store (shouldBeEnabled); }
    void setGain (float newGain)               { gain.store (newGain); }
    void push (const juce::AudioBuffer<float>& buffer);

    // Stream health: how close the output got to running out of audio, and dropouts.
    // Always measured (a few comparisons per output block). Starts again when the buffer
    // size changes or the device starts.
    std::optional<double> takeLowestMarginMs();             // lowest since the last call
    std::optional<double> getSessionLowestMarginMs() const; // lowest since the restart
    std::optional<double> getSecondsSinceLastDropout() const;

    // The copy's delay right now: audio waiting in the plugin (averaged), plus Windows'
    // output buffer for the device. The cable and Discord add their own delay after this.
    double getBufferedMs() const               { return averageFillSamples.load() * 1000.0 / hostSampleRate.load(); }
    double getWindowsOutputMs() const          { return windowsOutputMs.load(); }
    bool isLowLatencyMode() const              { return lowLatencyActive.load(); }

    // Any thread.
    bool isRunning() const                     { return running.load(); }
    double getTargetMs() const                 { return targetMs.load(); }
    double getMinimumMs() const                { return minimumMs.load(); }
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
    void audioDeviceListChanged() override;
    void resetHealth();
    juce::String tryOpen (juce::AudioIODeviceType& type, const juce::String& deviceName, bool lowLatency);

    // 1 MB. Fits the largest buffer setting (250 ms) at 192 kHz, with room to spare.
    static constexpr int fifoSize = 1 << 17;
    static constexpr double maxTargetSamples = fifoSize / 2.0;
    static constexpr double maxSpeedCorrection = 0.002;
    static constexpr double jitterMarginSamples = 32.0;

    std::unique_ptr<juce::AudioIODeviceType> deviceType;         // low-latency mode; also lists devices
    std::unique_ptr<juce::AudioIODeviceType> normalDeviceType;   // fallback, made only when needed
    std::unique_ptr<juce::AudioIODevice> device;
    juce::String openDeviceName;
    bool closingOnPurpose = false;      // message thread only
    juce::uint32 startTime = 0;         // message thread only

    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> fifoBuffer { 2, fifoSize };

    // Used only by the output device thread.
    juce::AudioBuffer<float> scratch, rendered;
    std::array<juce::LagrangeInterpolator, 2> interpolators;
    juce::LinearSmoothedValue<float> smoothedGain;
    double smoothedFill = 0.0;
    double lastTarget = 0.0;
    bool priming = true;

    std::atomic<double> hostSampleRate { 48000.0 };
    std::atomic<double> deviceSampleRate { 0.0 };
    std::atomic<int> hostBlockSize { 512 };
    std::atomic<float> targetLatencyMs { 40.0f };
    std::atomic<double> targetMs { 0.0 };
    std::atomic<double> minimumMs { 0.0 };
    std::atomic<bool> enabled { true };
    std::atomic<bool> running { false };
    std::atomic<float> gain { 1.0f };
    std::atomic<int> dropouts { 0 };
    std::atomic<juce::uint32> lastPushTime { 0 };
    std::atomic<double> averageFillSamples { 0.0 };
    std::atomic<double> windowsOutputMs { 0.0 };
    std::atomic<bool> lowLatencyActive { false };
    std::atomic<int> recentLowestMargin { INT_MAX };
    std::atomic<int> sessionLowestMargin { INT_MAX };
    std::atomic<juce::uint32> lastDropoutTime { 0 };

    mutable juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BridgeOutput)
};
