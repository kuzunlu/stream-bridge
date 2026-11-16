#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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

    juce::AudioProcessorValueTreeState parameters;
    BridgeOutput bridge;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamBridgeProcessor)
};
