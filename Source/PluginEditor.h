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
    void chooseDownloadFolder();
    void updateTuningDisplay();

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
    juce::TextButton tuneButton;
    juce::Label marginLabel, adviceLabel;
    std::array<std::optional<double>, 8> recentMargins;   // the last 2 s, one value per timer tick
    size_t nextMargin = 0;
    juce::Label statusLabel;

    juce::Label downloadHeading { {}, "YouTube to audio" };
    juce::Label linkLabel { {}, "Link" };
    juce::TextEditor linkEditor;
    juce::Label folderLabel { {}, "Save to" };
    juce::Label folderPath;
    juce::TextButton folderButton { "Choose..." };
    juce::Label formatLabel { {}, "Format" };
    juce::ComboBox formatBox;
    juce::TextButton downloadButton { "Download" };
    juce::TextButton updateButton { "Update yt-dlp" };
    double downloadProgress = 0.0;
    juce::ProgressBar progressBar { downloadProgress };
    juce::Label downloadStatus;
    std::unique_ptr<juce::FileChooser> folderChooser;

    juce::AudioProcessorValueTreeState::ButtonAttachment enabledAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment gainAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment bufferAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamBridgeEditor)
};
