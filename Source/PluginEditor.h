#pragma once

#include "PluginProcessor.h"

class StreamBridgeEditor final : public juce::AudioProcessorEditor,
                                 private juce::Timer
{
public:
    explicit StreamBridgeEditor (StreamBridgeProcessor&);
    ~StreamBridgeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshDeviceList();

    StreamBridgeProcessor& bridgeProcessor;
    juce::String shownSelection;

    juce::Label deviceLabel { {}, "Output" };
    juce::ComboBox deviceBox;
    juce::TextButton refreshButton { "Refresh" };
    juce::ToggleButton enabledButton { "Send audio" };
    juce::Label gainLabel { {}, "Volume" };
    juce::Slider gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label bufferLabel { {}, "Stream buffer" };
    juce::Slider bufferSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label statusLabel;

    juce::AudioProcessorValueTreeState::ButtonAttachment enabledAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment gainAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment bufferAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamBridgeEditor)
};
