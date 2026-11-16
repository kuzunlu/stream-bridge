#include "PluginEditor.h"
#include "WindowsAudioCheck.h"

#ifndef STREAM_BRIDGE_VERSION
 #define STREAM_BRIDGE_VERSION "dev"
#endif

namespace
{
    juce::String describeDuration (double seconds)
    {
        if (seconds < 60.0)
        {
            const auto s = juce::roundToInt (seconds);
            return juce::String (s) + (s == 1 ? " second" : " seconds");
        }

        const auto m = juce::roundToInt (seconds / 60.0);
        return juce::String (m) + (m == 1 ? " minute" : " minutes");
    }
}

void DraggableFileLabel::mouseDrag (const juce::MouseEvent&)
{
    if (dragging || getFile == nullptr)
        return;

    if (const auto file = getFile(); file.existsAsFile())
    {
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles (juce::StringArray (file.getFullPathName()), false, this,
                                                                    [safe = juce::Component::SafePointer<DraggableFileLabel> (this)]
                                                                    {
                                                                        if (safe != nullptr)
                                                                            safe->dragging = false;
                                                                    });
    }
}

StreamBridgeEditor::StreamBridgeEditor (StreamBridgeProcessor& p)
    : AudioProcessorEditor (p),
      bridgeProcessor (p),
      enabledAttachment (p.parameters, "enabled", enabledButton),
      gainAttachment (p.parameters, "gain", gainSlider),
      bufferAttachment (p.parameters, "bufferMs", bufferSlider)
{
    for (auto* c : std::initializer_list<juce::Component*> { &deviceLabel, &deviceBox, &refreshButton,
                                                             &enabledButton, &gainLabel, &gainSlider,
                                                             &bufferLabel, &bufferSlider, &detailsButton,
                                                             &healthLabel, &marginLabel, &adviceLabel,
                                                             &statusLabel, &warningLabel, &getCableButton,
                                                             &downloadHeading, &versionLabel, &linkLabel, &linkEditor,
                                                             &folderLabel, &folderPath, &folderButton,
                                                             &formatLabel, &formatBox, &downloadButton,
                                                             &updateButton, &progressBar, &downloadStatus,
                                                             &savedFileLabel, &showButton })
        addAndMakeVisible (c);

    // Streaming
    deviceBox.onChange = [this]
    {
        if (deviceBox.getSelectedItemIndex() >= 0)
            bridgeProcessor.selectOutputDevice (deviceBox.getText());

        timerCallback();
    };

    refreshButton.onClick = [this]
    {
        refreshDeviceList();

        // Also a way to reconnect by hand.
        if (const auto selected = bridgeProcessor.getSelectedOutputDevice(); selected.isNotEmpty() && ! bridgeProcessor.bridge.isRunning())
            bridgeProcessor.selectOutputDevice (selected);
        else
            bridgeProcessor.checkWindowsSettings();

        timerCallback();
    };

    gainSlider.setTextValueSuffix (" dB");
    bufferSlider.setTextValueSuffix (" ms");

    detailsButton.setClickingTogglesState (true);
    detailsButton.onClick = [this]
    {
        recentMargins.fill ({});
        timerCallback();
        updateLayout();
    };

    for (auto* label : { &healthLabel, &marginLabel, &adviceLabel, &statusLabel })
    {
        label->setJustificationType (juce::Justification::topLeft);
        label->setMinimumHorizontalScale (1.0f);
    }

    warningLabel.setJustificationType (juce::Justification::topLeft);
    warningLabel.setColour (juce::Label::textColourId, juce::Colours::orange);

    // Shown only when needed (see updateLayout).
    for (auto* c : std::initializer_list<juce::Component*> { &marginLabel, &adviceLabel, &warningLabel, &getCableButton,
                                                             &savedFileLabel, &showButton })
        c->setVisible (false);
    getCableButton.onClick = [] { juce::URL ("https://vb-audio.com/Cable/").launchInDefaultBrowser(); };

    // Downloader
    downloadHeading.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    versionLabel.setText (juce::String ("Stream Bridge ") + STREAM_BRIDGE_VERSION, juce::dontSendNotification);
    versionLabel.setJustificationType (juce::Justification::centredRight);
    versionLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    linkEditor.setTextToShowWhenEmpty ("Paste a YouTube link here", juce::Colours::grey);
    folderPath.setMinimumHorizontalScale (1.0f);
    folderButton.onClick = [this] { chooseDownloadFolder(); };

    formatBox.addItemList (AudioDownloader::getFormatNames(), 1);
    formatBox.setSelectedItemIndex ((int) bridgeProcessor.getDownloadFormat(), juce::dontSendNotification);
    formatBox.onChange = [this]
    {
        if (formatBox.getSelectedItemIndex() >= 0)
            bridgeProcessor.setDownloadFormat ((AudioDownloader::Format) formatBox.getSelectedItemIndex());
    };

    downloadButton.onClick = [this]
    {
        auto& downloader = bridgeProcessor.downloader;

        if (downloader.isBusy())
            downloader.cancel();
        else
            downloader.startDownload (linkEditor.getText(), bridgeProcessor.getDownloadFolder(), bridgeProcessor.getDownloadFormat());

        timerCallback();
    };

    updateButton.onClick = [this]
    {
        bridgeProcessor.downloader.startUpdate();
        timerCallback();
    };

    downloadStatus.setJustificationType (juce::Justification::topLeft);
    downloadStatus.setMinimumHorizontalScale (1.0f);

    savedFileLabel.getFile = [this] { return bridgeProcessor.downloader.getSavedFile(); };
    savedFileLabel.setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    savedFileLabel.setColour (juce::Label::outlineColourId, juce::Colours::grey);
    savedFileLabel.setMinimumHorizontalScale (1.0f);
    showButton.onClick = [this] { bridgeProcessor.downloader.getSavedFile().revealToUser(); };

    // Tooltips
    deviceBox.setTooltip ("Where the copy of your Master channel goes. Pick a virtual cable, like CABLE Input. "
                          "You should not hear this output yourself.");
    refreshButton.setTooltip ("Look for outputs again, for example after installing a virtual cable. "
                              "Also reconnects if the output stopped.");
    enabledButton.setTooltip ("On: viewers hear your DAW. Off: they hear nothing. Your own sound never changes.");
    gainSlider.setTooltip ("Volume of the copy only. Your own sound does not change.");
    bufferSlider.setTooltip ("How far behind the copy plays, so timing hiccups do not cause gaps. "
                             "Larger is safer, smaller has less delay for viewers. Your own sound is never delayed.");
    detailsButton.setTooltip ("Show the measurements behind the stream status.");
    healthLabel.setTooltip ("Whether viewers get the sound without gaps. A dropout is a short gap they hear.");
    statusLabel.setTooltip ("The output the copy goes to, its sample rate, and the stream buffer in use.");
    getCableButton.setTooltip ("Opens the VB-Audio website, where VB-Cable is free to download.");
    linkEditor.setTooltip ("A YouTube link. For a playlist link, only its first video is saved.");
    folderButton.setTooltip ("Choose where downloaded audio is saved.");
    formatBox.setTooltip ("YouTube's audio is about 128-160 kbps, so MP3 192 kbps keeps all of it. "
                          "FLAC and WAV are exact copies of what YouTube sends, in bigger files.");
    downloadButton.setTooltip ("Download the audio and save it in the chosen format.");
    updateButton.setTooltip ("Get the newest yt-dlp. A failed download also does this by itself, then tries again.");
    savedFileLabel.setTooltip ("Drag this onto an Ableton track.");
    showButton.setTooltip ("Open the folder with the saved file.");

    bridgeProcessor.checkWindowsSettings();
    refreshDeviceList();
    timerCallback();
    startTimerHz (4);

    setSize (editorWidth, layOut (false));
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

    noCableFound = std::none_of (names.begin(), names.end(), [] (const auto& name) { return WindowsAudioCheck::looksVirtual (name); });
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
    updateHealth();
    updateWarning();

    const auto& downloader = bridgeProcessor.downloader;
    const auto busy = downloader.isBusy();

    folderPath.setText (bridgeProcessor.getDownloadFolder().getFullPathName(), juce::dontSendNotification);
    downloadButton.setButtonText (busy ? "Cancel" : "Download");
    updateButton.setEnabled (! busy);
    downloadProgress = downloader.getProgress();
    downloadStatus.setText (downloader.getStatus(), juce::dontSendNotification);
    updateSavedFile();
}

void StreamBridgeEditor::updateHealth()
{
    auto& bridge = bridgeProcessor.bridge;
    const auto details = detailsButton.getToggleState();

    recentMargins[nextMargin] = bridge.takeLowestMarginMs();
    nextMargin = (nextMargin + 1) % recentMargins.size();

    const auto setLine = [] (juce::Label& label, const juce::String& text, juce::Colour colour)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, colour);
    };

    const auto green = juce::Colours::limegreen, orange = juce::Colours::orange, red = juce::Colours::red, grey = juce::Colours::grey;

    if (! bridge.isRunning())
    {
        setLine (healthLabel, {}, grey);
        setLine (marginLabel, {}, grey);
        setLine (adviceLabel, {}, grey);
        return;
    }

    if (bridgeProcessor.parameters.getRawParameterValue ("enabled")->load() < 0.5f)
    {
        setLine (healthLabel, "Send audio is off, so viewers hear nothing.", grey);
        setLine (marginLabel, {}, grey);
        setLine (adviceLabel, {}, grey);
        return;
    }

    // The buffer that would have left 2 ms to spare at the worst moment so far.
    std::optional<int> suggested;

    if (const auto lowest = bridge.getSessionLowestMarginMs())
        suggested = (int) std::ceil (juce::jmax (bridge.getMinimumMs(), bridge.getTargetMs() - *lowest + 2.0));

    const auto dropouts = bridge.getDropoutCount();
    const auto sinceDropout = bridge.getSecondsSinceLastDropout();
    const auto raise = suggested.has_value() && *suggested > juce::roundToInt (bridge.getTargetMs())
                         ? " Set the buffer to about " + juce::String (*suggested) + " ms."
                         : juce::String();

    if (dropouts == 0)
        setLine (healthLabel, "Stream OK, no dropouts.", green);
    else if (sinceDropout.has_value() && *sinceDropout < 10.0)
        setLine (healthLabel, "Dropout just now: viewers heard a short gap." + raise, red);
    else
        setLine (healthLabel, juce::String (dropouts) + (dropouts == 1 ? " dropout" : " dropouts") + ", the last one "
                                  + describeDuration (sinceDropout.value_or (0.0)) + " ago." + raise, orange);

    if (! details)
        return;

    std::optional<double> recentLowest;

    for (const auto& margin : recentMargins)
        if (margin.has_value() && (! recentLowest.has_value() || *margin < *recentLowest))
            recentLowest = margin;

    if (! recentLowest.has_value())
        setLine (marginLabel, "Safety margin: waiting for audio...", grey);
    else
        setLine (marginLabel, "Safety margin (last 2 seconds): " + juce::String (*recentLowest, 1) + " ms",
                 *recentLowest >= 3.0 ? green : *recentLowest >= 1.0 ? orange : red);

    auto advice = "Lowest possible buffer: " + juce::String (juce::roundToInt (std::ceil (bridge.getMinimumMs()))) + " ms";

    if (suggested.has_value())
        advice << "   Suggested so far: " << *suggested << " ms";

    setLine (adviceLabel, advice, getLookAndFeel().findColour (juce::Label::textColourId));
}

void StreamBridgeEditor::updateWarning()
{
    auto warning = bridgeProcessor.getWarningText();

    if (warning.isEmpty() && noCableFound)
        warning = "No virtual cable found. Install the free VB-Cable, restart the PC, then pick CABLE Input above.";

    const auto changed = warning != warningLabel.getText() || noCableFound != getCableButton.isVisible();

    warningLabel.setText (warning, juce::dontSendNotification);
    warningLabel.setVisible (warning.isNotEmpty());
    getCableButton.setVisible (noCableFound);

    if (changed)
        updateLayout();
}

void StreamBridgeEditor::updateSavedFile()
{
    const auto file = bridgeProcessor.downloader.isBusy() ? juce::File() : bridgeProcessor.downloader.getSavedFile();
    const auto show = file.existsAsFile();

    savedFileLabel.setText (show ? "Drag into Ableton:  " + file.getFileName() : juce::String(), juce::dontSendNotification);

    if (show != savedFileLabel.isVisible())
    {
        savedFileLabel.setVisible (show);
        showButton.setVisible (show);
        updateLayout();
    }
}

void StreamBridgeEditor::updateLayout()
{
    marginLabel.setVisible (detailsButton.getToggleState());
    adviceLabel.setVisible (detailsButton.getToggleState());

    if (const auto height = layOut (false); height != getHeight())
        setSize (editorWidth, height);
    else
        resized();
}

void StreamBridgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (getLookAndFeel().findColour (juce::Label::textColourId).withAlpha (0.2f));
    g.fillRect (12, downloadHeading.getY() - 8, getWidth() - 24, 1);
}

void StreamBridgeEditor::resized()
{
    layOut (true);
}

int StreamBridgeEditor::layOut (bool apply)
{
    const auto place = [apply] (juce::Component& component, juce::Rectangle<int> bounds)
    {
        if (apply)
            component.setBounds (bounds);
    };

    auto area = juce::Rectangle<int> (0, 0, editorWidth, 10000).reduced (12);

    auto row = area.removeFromTop (28);
    place (deviceLabel, row.removeFromLeft (60));
    place (refreshButton, row.removeFromRight (80));
    row.removeFromRight (8);
    place (deviceBox, row);

    area.removeFromTop (10);
    row = area.removeFromTop (28);
    place (enabledButton, row.removeFromLeft (120));
    place (gainLabel, row.removeFromLeft (60));
    place (gainSlider, row);

    area.removeFromTop (10);
    row = area.removeFromTop (28);
    place (bufferLabel, row.removeFromLeft (120));
    place (detailsButton, row.removeFromRight (70));
    row.removeFromRight (8);
    place (bufferSlider, row);

    area.removeFromTop (6);
    place (healthLabel, area.removeFromTop (36));

    if (detailsButton.getToggleState())
    {
        place (marginLabel, area.removeFromTop (20));
        place (adviceLabel, area.removeFromTop (20));
    }

    area.removeFromTop (4);
    place (statusLabel, area.removeFromTop (36));

    if (warningLabel.isVisible() || getCableButton.isVisible())
    {
        area.removeFromTop (4);
        row = area.removeFromTop (52);

        if (getCableButton.isVisible())
        {
            place (getCableButton, row.removeFromRight (100).removeFromTop (28));
            row.removeFromRight (8);
        }

        place (warningLabel, row);
    }

    area.removeFromTop (18);
    row = area.removeFromTop (24);
    place (versionLabel, row.removeFromRight (180));
    place (downloadHeading, row);

    area.removeFromTop (6);
    row = area.removeFromTop (28);
    place (linkLabel, row.removeFromLeft (60));
    place (linkEditor, row);

    area.removeFromTop (8);
    row = area.removeFromTop (28);
    place (folderLabel, row.removeFromLeft (60));
    place (folderButton, row.removeFromRight (80));
    row.removeFromRight (8);
    place (folderPath, row);

    area.removeFromTop (8);
    row = area.removeFromTop (28);
    place (formatLabel, row.removeFromLeft (60));
    place (updateButton, row.removeFromRight (100));
    row.removeFromRight (8);
    place (downloadButton, row.removeFromRight (90));
    row.removeFromRight (8);
    place (formatBox, row);

    area.removeFromTop (10);
    place (progressBar, area.removeFromTop (18));

    area.removeFromTop (6);
    place (downloadStatus, area.removeFromTop (36));

    if (savedFileLabel.isVisible())
    {
        area.removeFromTop (4);
        row = area.removeFromTop (28);
        place (showButton, row.removeFromRight (110));
        row.removeFromRight (8);
        place (savedFileLabel, row);
    }

    return area.getY() + 12;
}
