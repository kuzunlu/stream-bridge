#pragma once

#include "PluginProcessor.h"

// A label showing a file that can be dragged out of the window, for example onto an
// Ableton track.
class DraggableFileLabel final : public juce::Label
{
public:
    std::function<juce::File()> getFile;

    void mouseDrag (const juce::MouseEvent&) override;

private:
    bool dragging = false;
};

class StreamBridgeEditor final : public juce::AudioProcessorEditor,
                                 private juce::Timer
{
public:
    explicit StreamBridgeEditor (StreamBridgeProcessor&);
    ~StreamBridgeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int editorWidth = 480;

    void timerCallback() override;
    void refreshDeviceList();
    void chooseDownloadFolder();
    void updateHealth();
    void clearHealthHistory();
    void updateWarning();
    void updateSavedFile();
    void updateLayout();
    int layOut (bool apply);   // returns the height everything needs

    StreamBridgeProcessor& bridgeProcessor;
    juce::String shownSelection;
    bool noCableFound = false;

    juce::TooltipWindow tooltipWindow { this, 600 };

    juce::Label deviceLabel { {}, "Output" };
    juce::ComboBox deviceBox;
    juce::TextButton refreshButton { "Refresh" };
    juce::ToggleButton enabledButton { "Send audio" };
    juce::Label gainLabel { {}, "Volume" };
    juce::Slider gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label bufferLabel { {}, "Stream buffer" };
    juce::Slider bufferSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label healthLabel, adviceLabel, totalLabel, delayLabel;
    juce::Label statusLabel;

    // Stream health for the last 10 seconds, one entry per second.
    static constexpr size_t historySize = 10;
    std::array<std::optional<double>, historySize> recentMargins;
    std::array<int, historySize> recentDropouts {};
    size_t nextSecond = 0, secondsMeasured = 0;
    double historyTarget = -1.0;
    int lastDropoutCount = 0;
    int timerTicks = 0;
    juce::Label warningLabel;
    juce::TextButton getCableButton { "Get VB-Cable" };

    juce::Label downloadHeading { {}, "YouTube to audio" };
    juce::Label versionLabel;
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
    DraggableFileLabel savedFileLabel;
    juce::TextButton showButton { "Show in folder" };
    std::unique_ptr<juce::FileChooser> folderChooser;

    juce::AudioProcessorValueTreeState::ButtonAttachment enabledAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment gainAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment bufferAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamBridgeEditor)
};
