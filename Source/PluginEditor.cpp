#include "PluginEditor.h"

StreamBridgeEditor::StreamBridgeEditor (StreamBridgeProcessor& p)
    : AudioProcessorEditor (p),
      bridgeProcessor (p),
      enabledAttachment (p.parameters, "enabled", enabledButton),
      gainAttachment (p.parameters, "gain", gainSlider),
      bufferAttachment (p.parameters, "bufferMs", bufferSlider)
{
    for (auto* c : std::initializer_list<juce::Component*> { &deviceLabel, &deviceBox, &refreshButton,
                                                             &enabledButton, &gainLabel, &gainSlider,
                                                             &bufferLabel, &bufferSlider, &tuneButton,
                                                             &marginLabel, &adviceLabel, &statusLabel,
                                                             &downloadHeading, &linkLabel, &linkEditor,
                                                             &folderLabel, &folderPath, &folderButton,
                                                             &formatLabel, &formatBox, &downloadButton,
                                                             &updateButton, &progressBar, &downloadStatus })
        addAndMakeVisible (c);

    deviceBox.onChange = [this]
    {
        if (deviceBox.getSelectedItemIndex() >= 0)
            bridgeProcessor.selectOutputDevice (deviceBox.getText());
    };

    refreshButton.onClick = [this] { refreshDeviceList(); };

    gainSlider.setTextValueSuffix (" dB");
    bufferSlider.setTextValueSuffix (" ms");
    statusLabel.setJustificationType (juce::Justification::topLeft);
    statusLabel.setMinimumHorizontalScale (1.0f);

    tuneButton.onClick = [this]
    {
        auto& bridge = bridgeProcessor.bridge;
        bridge.setMonitoring (! bridge.isMonitoring());
        recentMargins.fill ({});
        timerCallback();
    };

    downloadHeading.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    linkEditor.setTextToShowWhenEmpty ("Paste a YouTube link here", juce::Colours::grey);
    folderPath.setMinimumHorizontalScale (1.0f);
    folderButton.onClick = [this] { chooseDownloadFolder(); };

    const auto formats = AudioDownloader::getFormatNames();
    formatBox.addItemList (formats, 1);
    formatBox.setSelectedItemIndex (juce::jlimit (0, formats.size() - 1, bridgeProcessor.getDownloadFormat()), juce::dontSendNotification);
    formatBox.onChange = [this] { bridgeProcessor.setDownloadFormat (formatBox.getSelectedItemIndex()); };

    downloadButton.onClick = [this]
    {
        auto& downloader = bridgeProcessor.downloader;

        if (downloader.isBusy())
            downloader.cancel();
        else
            downloader.startDownload (linkEditor.getText(), bridgeProcessor.getDownloadFolder(),
                                      (AudioDownloader::Format) juce::jmax (0, formatBox.getSelectedItemIndex()));

        timerCallback();
    };

    updateButton.onClick = [this]
    {
        bridgeProcessor.downloader.startUpdate();
        timerCallback();
    };

    downloadStatus.setJustificationType (juce::Justification::topLeft);
    downloadStatus.setMinimumHorizontalScale (1.0f);

    refreshDeviceList();
    timerCallback();
    startTimerHz (4);

    setSize (480, 470);
}

StreamBridgeEditor::~StreamBridgeEditor()
{
    stopTimer();
}

void StreamBridgeEditor::refreshDeviceList()
{
    const auto names = bridgeProcessor.bridge.getOutputDeviceNames();

    deviceBox.clear (juce::dontSendNotification);
    deviceBox.addItemList (names, 1);

    const auto index = names.indexOf (bridgeProcessor.getSelectedOutputDevice());

    if (index >= 0)
        deviceBox.setSelectedItemIndex (index, juce::dontSendNotification);
}

void StreamBridgeEditor::chooseDownloadFolder()
{
    folderChooser = std::make_unique<juce::FileChooser> ("Choose where to save the audio", bridgeProcessor.getDownloadFolder());

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [this] (const juce::FileChooser& chooser)
                                {
                                    if (const auto folder = chooser.getResult(); folder != juce::File())
                                        bridgeProcessor.setDownloadFolder (folder);

                                    timerCallback();
                                });
}

void StreamBridgeEditor::timerCallback()
{
    // The device may be opened a moment after the window appears.
    if (const auto selected = bridgeProcessor.getSelectedOutputDevice(); selected != shownSelection)
    {
        shownSelection = selected;
        refreshDeviceList();
    }

    statusLabel.setText (bridgeProcessor.getStatusText(), juce::dontSendNotification);
    updateTuningDisplay();

    const auto& downloader = bridgeProcessor.downloader;
    const auto busy = downloader.isBusy();

    folderPath.setText (bridgeProcessor.getDownloadFolder().getFullPathName(), juce::dontSendNotification);
    downloadButton.setButtonText (busy ? "Cancel" : "Download");
    updateButton.setEnabled (! busy);
    downloadProgress = downloader.getProgress();
    downloadStatus.setText (downloader.getStatus(), juce::dontSendNotification);
}

void StreamBridgeEditor::updateTuningDisplay()
{
    auto& bridge = bridgeProcessor.bridge;
    const auto tuning = bridge.isMonitoring();

    tuneButton.setButtonText (tuning ? "Lock" : "Tune");
    bufferSlider.setEnabled (tuning);
    marginLabel.setVisible (tuning);
    adviceLabel.setVisible (tuning);

    // When locked, nothing is measured, so there is nothing to show.
    if (! tuning)
        return;

    recentMargins[nextMargin] = bridge.takeLowestMarginMs();
    nextMargin = (nextMargin + 1) % recentMargins.size();

    std::optional<double> lowest;

    for (const auto& margin : recentMargins)
        if (margin.has_value() && (! lowest.has_value() || *margin < *lowest))
            lowest = margin;

    if (! lowest.has_value())
    {
        marginLabel.setText ("Safety margin: waiting for audio...", juce::dontSendNotification);
        marginLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    }
    else
    {
        const auto colour = *lowest >= 3.0 ? juce::Colours::limegreen
                          : *lowest >= 1.0 ? juce::Colours::orange
                                           : juce::Colours::red;

        marginLabel.setText ("Safety margin (last 2 s): " + juce::String (*lowest, 1) + " ms", juce::dontSendNotification);
        marginLabel.setColour (juce::Label::textColourId, colour);
    }

    juce::String advice;

    if (const auto sinceDropout = bridge.getSecondsSinceLastDropout())
        advice << "Dropouts: " << bridge.getDropoutCount() << ", last one " << juce::roundToInt (*sinceDropout) << " s ago.  ";

    // Suggest the buffer that would have left 2 ms to spare at the worst moment so far.
    if (const auto sessionLowest = bridge.getSessionLowestMarginMs())
    {
        const auto suggested = juce::jmax (bridge.getMinimumMs(), bridge.getTargetMs() - *sessionLowest + 2.0);
        advice << "Suggested buffer so far: " << juce::roundToInt (std::ceil (suggested)) << " ms";
    }

    const auto recentDropout = bridge.getSecondsSinceLastDropout().value_or (1.0e9) < 5.0;
    adviceLabel.setText (advice, juce::dontSendNotification);
    adviceLabel.setColour (juce::Label::textColourId, recentDropout ? juce::Colours::red
                                                                    : getLookAndFeel().findColour (juce::Label::textColourId));
}

void StreamBridgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (getLookAndFeel().findColour (juce::Label::textColourId).withAlpha (0.2f));
    g.fillRect (12, downloadHeading.getY() - 8, getWidth() - 24, 1);
}

void StreamBridgeEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto row = area.removeFromTop (28);
    deviceLabel.setBounds (row.removeFromLeft (60));
    refreshButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (8);
    deviceBox.setBounds (row);

    area.removeFromTop (10);
    row = area.removeFromTop (28);
    enabledButton.setBounds (row.removeFromLeft (120));
    gainLabel.setBounds (row.removeFromLeft (60));
    gainSlider.setBounds (row);

    area.removeFromTop (10);
    row = area.removeFromTop (28);
    bufferLabel.setBounds (row.removeFromLeft (180));
    tuneButton.setBounds (row.removeFromRight (60));
    row.removeFromRight (8);
    bufferSlider.setBounds (row);

    area.removeFromTop (4);
    marginLabel.setBounds (area.removeFromTop (20));
    adviceLabel.setBounds (area.removeFromTop (20));

    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (40));

    area.removeFromTop (16);
    downloadHeading.setBounds (area.removeFromTop (24));

    area.removeFromTop (6);
    row = area.removeFromTop (28);
    linkLabel.setBounds (row.removeFromLeft (60));
    linkEditor.setBounds (row);

    area.removeFromTop (8);
    row = area.removeFromTop (28);
    folderLabel.setBounds (row.removeFromLeft (60));
    folderButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (8);
    folderPath.setBounds (row);

    area.removeFromTop (8);
    row = area.removeFromTop (28);
    formatLabel.setBounds (row.removeFromLeft (60));
    updateButton.setBounds (row.removeFromRight (100));
    row.removeFromRight (8);
    downloadButton.setBounds (row.removeFromRight (90));
    row.removeFromRight (8);
    formatBox.setBounds (row);

    area.removeFromTop (10);
    progressBar.setBounds (area.removeFromTop (18));

    area.removeFromTop (6);
    downloadStatus.setBounds (area);
}
