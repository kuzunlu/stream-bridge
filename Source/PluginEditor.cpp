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
                                                             &bufferLabel, &bufferSlider, &statusLabel })
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

    refreshDeviceList();
    timerCallback();
    startTimerHz (4);

    setSize (480, 210);
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

void StreamBridgeEditor::timerCallback()
{
    // The device may be opened a moment after the window appears.
    if (const auto selected = bridgeProcessor.getSelectedOutputDevice(); selected != shownSelection)
    {
        shownSelection = selected;
        refreshDeviceList();
    }

    statusLabel.setText (bridgeProcessor.getStatusText(), juce::dontSendNotification);
}

void StreamBridgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
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
    bufferSlider.setBounds (row);

    area.removeFromTop (10);
    statusLabel.setBounds (area);
}
