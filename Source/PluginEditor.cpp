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
                                                             &bufferLabel, &bufferSlider,
                                                             &healthLabel, &adviceLabel, &totalLabel, &delayLabel,
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

    for (auto* label : { &healthLabel, &adviceLabel, &totalLabel, &delayLabel, &statusLabel })
    {
        label->setJustificationType (juce::Justification::topLeft);
        label->setMinimumHorizontalScale (1.0f);
    }

    // The line that says whether the stream is stable stands out.
    healthLabel.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    adviceLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));

    warningLabel.setJustificationType (juce::Justification::topLeft);
    warningLabel.setColour (juce::Label::textColourId, juce::Colours::orange);

    // Shown only when needed (see updateLayout).
    for (auto* c : std::initializer_list<juce::Component*> { &warningLabel, &getCableButton, &savedFileLabel, &showButton })
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
    enabledButton.setTooltip ("On: viewers hear your DAW. Off: they hear nothing. Your own sound never changes. "
                              "Put Stream Bridge last on the Master channel, so viewers also hear the effects before it.");
    gainSlider.setTooltip ("Volume of the copy only. Your own sound does not change.");
    bufferSlider.setTooltip ("How far behind the copy plays, so timing hiccups do not cause gaps. "
                             "Larger is safer, smaller has less delay for viewers. Your own sound is never delayed.");
    healthLabel.setTooltip ("Spare audio is how much sound is still waiting each time Windows asks for more. "
                            "If it runs out, viewers hear a short gap (a dropout). This judges the last 10 seconds, "
                            "so play the way you will on stream while you watch it.");
    adviceLabel.setTooltip ("The Stream buffer that keeps at least 3 ms of spare audio, based on the last 10 seconds.");
    totalLabel.setTooltip ("Dropouts since you last changed the Stream buffer. Restarts and reconnects do not reset this.");
    delayLabel.setTooltip ("How far behind your own sound the copy is when it leaves this plugin. "
                           "The cable and Discord add their own delay after that.");
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
    bridgeProcessor.bridge.takeLowestMarginMs();    // forget what happened while the window was closed
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

    // The stream lines change once per second. The downloader below keeps the faster timer.
    if (timerTicks++ % 4 == 0)
    {
        statusLabel.setText (bridgeProcessor.getStatusText(), juce::dontSendNotification);
        updateHealth();
    }

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

void StreamBridgeEditor::clearHealthHistory()
{
    recentMargins.fill ({});
    recentDropouts.fill (0);
    nextSecond = 0;
    secondsMeasured = 0;
    lastDropoutCount = bridgeProcessor.bridge.getDropoutCount();
    bridgeProcessor.bridge.takeLowestMarginMs();
}

// Called once per second.
void StreamBridgeEditor::updateHealth()
{
    auto& bridge = bridgeProcessor.bridge;

    const auto setLine = [] (juce::Label& label, const juce::String& text, juce::Colour colour)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, colour);
    };

    const auto green = juce::Colours::limegreen, orange = juce::Colours::orange, red = juce::Colours::red, grey = juce::Colours::grey;
    const auto textColour = getLookAndFeel().findColour (juce::Label::textColourId);

    const auto showOnly = [&] (const juce::String& text)
    {
        setLine (healthLabel, text, grey);
        setLine (adviceLabel, {}, grey);
        setLine (totalLabel, {}, grey);
        setLine (delayLabel, {}, grey);
        bufferLabel.setColour (juce::Label::textColourId, textColour);
        historyTarget = -1.0;
    };

    if (! bridge.isRunning())
        return showOnly ({});

    if (bridgeProcessor.parameters.getRawParameterValue ("enabled")->load() < 0.5f)
        return showOnly ("Send audio is off, so viewers hear nothing.");

    // Add this second to the last 10. Changing the Stream buffer starts the count over.
    const auto target = bridge.getTargetMs();
    const auto setting = (double) bridgeProcessor.parameters.getRawParameterValue ("bufferMs")->load();
    const auto dropoutCount = bridge.getDropoutCount();

    if (setting != historyTarget || dropoutCount < lastDropoutCount)
    {
        historyTarget = setting;
        clearHealthHistory();
    }
    else
    {
        recentMargins[nextSecond] = bridge.takeLowestMarginMs();
        recentDropouts[nextSecond] = dropoutCount - lastDropoutCount;
        lastDropoutCount = dropoutCount;
        nextSecond = (nextSecond + 1) % historySize;
        secondsMeasured = juce::jmin (secondsMeasured + 1, historySize);
    }

    std::optional<double> lowest;
    int dropouts = 0;

    for (size_t i = 0; i < historySize; ++i)
    {
        dropouts += recentDropouts[i];

        if (recentMargins[i].has_value() && (! lowest.has_value() || *recentMargins[i] < *lowest))
            lowest = recentMargins[i];
    }

    const auto window = secondsMeasured >= historySize ? juce::String ("last 10 seconds")
                                                       : "last " + describeDuration ((double) secondsMeasured);
    const auto current = juce::roundToInt (target);
    const auto minimum = (int) std::ceil (bridge.getMinimumMs());

    if (! lowest.has_value())
    {
        setLine (healthLabel, "Measuring the stream...", grey);
        setLine (adviceLabel, "What to do: play something and wait 10 seconds.", grey);
        bufferLabel.setColour (juce::Label::textColourId, textColour);
    }
    else
    {
        // The buffer that would have kept 3 ms of spare audio at the worst moment.
        const auto keepsThreeMs = juce::jmax (minimum, (int) std::ceil (target - *lowest + 3.0));
        const auto raiseTo = juce::jmax (keepsThreeMs, current + 1);
        const auto spare = juce::String (*lowest, 1) + " ms of spare audio (" + window + ").";
        const auto raise = "What to do: raise the Stream buffer to " + juce::String (raiseTo) + " ms.";

        if (dropouts > 0)
        {
            setLine (healthLabel, "Gaps: " + juce::String (dropouts) + (dropouts == 1 ? " dropout" : " dropouts")
                                      + " in the " + window + ". Viewers heard short gaps.", red);
            setLine (adviceLabel, raise, red);
            bufferLabel.setColour (juce::Label::textColourId, red);
        }
        else if (*lowest < 1.0)
        {
            setLine (healthLabel, "Almost a gap: only " + spare, red);
            setLine (adviceLabel, raise, red);
            bufferLabel.setColour (juce::Label::textColourId, red);
        }
        else if (*lowest < 3.0)
        {
            setLine (healthLabel, "Close to gaps: only " + spare, orange);
            setLine (adviceLabel, raise, orange);
            bufferLabel.setColour (juce::Label::textColourId, orange);
        }
        else
        {
            // Suggest a lower buffer only after a full 10 seconds of measuring, and never after
            // a dropout at this buffer value: whatever caused it can happen again.
            const auto canLower = secondsMeasured >= historySize && keepsThreeMs < current && bridge.getDropoutCount() == 0;

            const auto advice = secondsMeasured < historySize ? juce::String ("What to do: keep playing, the full check takes 10 seconds.")
                              : canLower                      ? "What to do: nothing. For less delay you can lower the Stream buffer to "
                                                                    + juce::String (keepsThreeMs) + " ms."
                                                              : juce::String ("What to do: nothing.");

            setLine (healthLabel, "Stream stable: " + spare, green);
            setLine (adviceLabel, advice, green);
            bufferLabel.setColour (juce::Label::textColourId, textColour);
        }
    }

    // Everything since the Stream buffer was last changed.
    const auto totalDropouts = bridge.getDropoutCount();
    juce::String total ("Since the Stream buffer was last changed: ");

    if (totalDropouts == 0)
        total << "no dropouts.";
    else
        total << totalDropouts << (totalDropouts == 1 ? " dropout" : " dropouts") << ", the last one "
              << describeDuration (bridge.getSecondsSinceLastDropout().value_or (0.0)) << " ago.";

    total << "\nLowest possible buffer: " << minimum << " ms";
    setLine (totalLabel, total, textColour);

    // What the plugin itself adds to the copy right now. The cable and Discord add theirs after it.
    const auto pluginMs = bridge.getBufferedMs();
    const auto windowsMs = bridge.getWindowsOutputMs();
    setLine (delayLabel, "Copy delay now: about " + juce::String (juce::roundToInt (pluginMs + windowsMs)) + " ms (plugin "
                             + juce::String (juce::roundToInt (pluginMs)) + " + Windows " + juce::String (juce::roundToInt (windowsMs))
                             + ", " + (bridge.isLowLatencyMode() ? "low-latency mode)" : "normal mode)"),
             textColour);
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
    place (bufferSlider, row);

    area.removeFromTop (8);
    place (healthLabel, area.removeFromTop (44));
    place (adviceLabel, area.removeFromTop (38));
    area.removeFromTop (4);
    place (totalLabel, area.removeFromTop (56));
    place (delayLabel, area.removeFromTop (20));

    area.removeFromTop (4);
    place (statusLabel, area.removeFromTop (36));

    if (warningLabel.isVisible() || getCableButton.isVisible())
    {
        // Tall enough for the whole warning, so long ones are never squeezed or cut off.
        juce::AttributedString text (warningLabel.getText());
        text.setFont (warningLabel.getFont());
        juce::TextLayout textLayout;
        textLayout.createLayout (text, (float) (area.getWidth() - (getCableButton.isVisible() ? 108 : 0)
                                                - warningLabel.getBorderSize().getLeftAndRight()));

        area.removeFromTop (4);
        row = area.removeFromTop (juce::jmax (28, (int) std::ceil (textLayout.getHeight())
                                                      + warningLabel.getBorderSize().getTopAndBottom() + 4));

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
