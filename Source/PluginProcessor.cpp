#include "PluginProcessor.h"
#include "PluginEditor.h"

StreamBridgeProcessor::StreamBridgeProcessor()
    : AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "StreamBridge", createParameterLayout())
{
    enabledParam = parameters.getRawParameterValue ("enabled");
    gainParam    = parameters.getRawParameterValue ("gain");
    bufferParam  = parameters.getRawParameterValue ("bufferMs");

    bridge.onReconnectNeeded = [this] { triggerAsyncUpdate(); };

    // Open the device a moment later, so a saved device from setStateInformation()
    // is used instead of opening a default one first.
    triggerAsyncUpdate();
}

StreamBridgeProcessor::~StreamBridgeProcessor()
{
    bridge.onReconnectNeeded = nullptr;
    cancelPendingUpdate();
    downloader.cancel();
    bridge.closeDevice();
}

juce::AudioProcessorValueTreeState::ParameterLayout StreamBridgeProcessor::createParameterLayout()
{
    return { std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "enabled", 1 }, "Send Audio", true),
             std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Volume",
                                                          juce::NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f,
                                                          juce::AudioParameterFloatAttributes().withLabel ("dB")),
             std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "bufferMs", 1 }, "Stream Buffer",
                                                          juce::NormalisableRange<float> (0.0f, 1000.0f, 1.0f, 0.3f), 40.0f,
                                                          juce::AudioParameterFloatAttributes().withLabel ("ms")) };
}

void StreamBridgeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    bridge.setHostSampleRate (sampleRate);
    bridge.setHostBlockSize (samplesPerBlock);

    // The device may have been opened before the host said its rate. If so, open it again
    // at the host's rate when the device allows that, so the copy is not resampled.
    if (bridge.isRunning() && sampleRate != bridge.getDeviceSampleRate())
        triggerAsyncUpdate();
}

bool StreamBridgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void StreamBridgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // The host's audio passes through untouched. Only the copy is sent.
    bridge.setEnabled (enabledParam->load() > 0.5f);
    bridge.setGain (juce::Decibels::decibelsToGain (gainParam->load(), -60.0f));
    bridge.setTargetLatencyMs (bufferParam->load());

    // When Ableton exports (renders offline) it runs faster than real time, so skip.
    if (! isNonRealtime())
        bridge.push (buffer);
}

juce::AudioProcessorEditor* StreamBridgeProcessor::createEditor()
{
    return new StreamBridgeEditor (*this);
}

void StreamBridgeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("outputDevice", getSelectedOutputDevice(), nullptr);
    state.setProperty ("downloadFolder", getDownloadFolder().getFullPathName(), nullptr);
    state.setProperty ("downloadFormat", getDownloadFormat(), nullptr);
    state.setProperty ("bufferTuning", bridge.isMonitoring(), nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void StreamBridgeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    parameters.replaceState (state);

    {
        const juce::ScopedLock sl (deviceLock);
        selectedDevice = state.getProperty ("outputDevice").toString();

        if (const auto folder = state.getProperty ("downloadFolder").toString(); juce::File::isAbsolutePath (folder))
            downloadFolder = juce::File (folder);

        downloadFormat = state.getProperty ("downloadFormat", 0);
    }

    bridge.setMonitoring (state.getProperty ("bufferTuning", false));

    triggerAsyncUpdate();
}

void StreamBridgeProcessor::selectOutputDevice (const juce::String& deviceName)
{
    {
        const juce::ScopedLock sl (deviceLock);
        selectedDevice = deviceName;
    }

    const auto error = bridge.openDevice (deviceName);

    const juce::ScopedLock sl (deviceLock);
    openError = error;
}

juce::String StreamBridgeProcessor::getSelectedOutputDevice() const
{
    const juce::ScopedLock sl (deviceLock);
    return selectedDevice;
}

juce::String StreamBridgeProcessor::getStatusText() const
{
    if (bridge.isRunning())
    {
        auto text = "Playing to " + bridge.getOpenDeviceName()
                  + "  |  " + juce::String (juce::roundToInt (bridge.getDeviceSampleRate())) + " Hz"
                  + "  |  buffer " + juce::String (juce::roundToInt (bridge.getTargetMs())) + " ms";

        if (bridge.isMonitoring())
            text << " (lowest possible " << juce::roundToInt (bridge.getMinimumMs()) << " ms)"
                 << "  |  dropouts: " << bridge.getDropoutCount();
        else
            text << " (locked)";

        if (const auto error = bridge.getLastError(); error.isNotEmpty())
            text << "  |  " << error;

        return text;
    }

    const juce::ScopedLock sl (deviceLock);

    if (openError.isNotEmpty())
        return "Error: " + openError;

    if (selectedDevice.isEmpty())
        return "No virtual cable found. Pick an output above.";

    return "Waiting for " + selectedDevice + " to come back...";
}

juce::File StreamBridgeProcessor::getDownloadFolder() const
{
    const juce::ScopedLock sl (deviceLock);
    return downloadFolder;
}

void StreamBridgeProcessor::setDownloadFolder (const juce::File& folder)
{
    const juce::ScopedLock sl (deviceLock);
    downloadFolder = folder;
}

int StreamBridgeProcessor::getDownloadFormat() const
{
    const juce::ScopedLock sl (deviceLock);
    return downloadFormat;
}

void StreamBridgeProcessor::setDownloadFormat (int formatIndex)
{
    const juce::ScopedLock sl (deviceLock);
    downloadFormat = formatIndex;
}

juce::String StreamBridgeProcessor::findVirtualCable()
{
    const auto names = bridge.getOutputDeviceNames();

    for (const auto& keyword : { "CABLE Input", "CABLE", "Virtual" })
        for (const auto& name : names)
            if (name.containsIgnoreCase (keyword))
                return name;

    return {};
}

// Opens the device when it is not playing yet, after it was lost, or when it could run at
// the host's sample rate. Does nothing while everything already works.
void StreamBridgeProcessor::handleAsyncUpdate()
{
    auto deviceName = getSelectedOutputDevice();

    if (deviceName.isEmpty())
        deviceName = findVirtualCable();

    if (deviceName.isEmpty())
        return;

    if (deviceName == bridge.getOpenDeviceName() && bridge.isRunning() && ! bridge.couldUseHostSampleRate())
        return;

    selectOutputDevice (deviceName);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StreamBridgeProcessor();
}
