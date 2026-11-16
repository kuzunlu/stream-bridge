#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "AudioDownloader.h"
#include "BridgeOutput.h"

class StreamBridgeProcessor final : public juce::AudioProcessor,
                                    private juce::AsyncUpdater
{
public:
    StreamBridgeProcessor();
    ~StreamBridgeProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                                 { return true; }

    const juce::String getName() const override                     { return JucePlugin_Name; }
    bool acceptsMidi() const override                               { return false; }
    bool producesMidi() const override                              { return false; }
    double getTailLengthSeconds() const override                    { return 0.0; }

    int getNumPrograms() override                                   { return 1; }
    int getCurrentProgram() override                                { return 0; }
    void setCurrentProgram (int) override                           {}
    const juce::String getProgramName (int) override                { return {}; }
    void changeProgramName (int, const juce::String&) override      {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Message thread only.
    void selectOutputDevice (const juce::String& deviceName);
    juce::String getSelectedOutputDevice() const;
    juce::String getStatusText() const;

    // A Windows setting that makes the user hear the copy, or empty. Checked when an
    // output is opened, and again by checkWindowsSettings().
    void checkWindowsSettings();
    juce::String getWarningText() const;

    juce::File getDownloadFolder() const;
    void setDownloadFolder (const juce::File& folder);
    AudioDownloader::Format getDownloadFormat() const;
    void setDownloadFormat (AudioDownloader::Format format);

    juce::AudioProcessorValueTreeState parameters;
    BridgeOutput bridge;
    AudioDownloader downloader;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::String findVirtualCable();
    void handleAsyncUpdate() override;

    std::atomic<float>* enabledParam = nullptr;
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* bufferParam = nullptr;

    mutable juce::CriticalSection deviceLock;
    juce::String selectedDevice;
    juce::String openError;
    juce::String warningText;
    juce::File downloadFolder { juce::File::getSpecialLocation (juce::File::userMusicDirectory) };
    AudioDownloader::Format downloadFormat = AudioDownloader::Format::mp3_192;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamBridgeProcessor)
};
