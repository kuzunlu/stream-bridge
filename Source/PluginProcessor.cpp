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

    // Open the device a moment later, so a saved device from setStateInformation()
    // is used instead of opening a default one first.
    triggerAsyncUpdate();
}

StreamBridgeProcessor::~StreamBridgeProcessor()
{
    cancelPendingUpdate();
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
    }

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
                  + "  |  buffer " + juce::String (juce::roundToInt (bridge.getTargetMs())) + " ms"
                  + " (lowest possible " + juce::String (juce::roundToInt (bridge.getMinimumMs())) + " ms)"
                  + "  |  dropouts: " + juce::String (bridge.getDropoutCount());

        if (const auto error = bridge.getLastError(); error.isNotEmpty())
            text << "  |  " << error;

        return text;
    }

    const juce::ScopedLock sl (deviceLock);

    if (openError.isNotEmpty())
        return "Error: " + openError;

    if (selectedDevice.isEmpty())
        return "No virtual cable found. Pick an output above.";

    return "Not running";
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

void StreamBridgeProcessor::handleAsyncUpdate()
{
    auto deviceName = getSelectedOutputDevice();

    if (deviceName.isEmpty())
        deviceName = findVirtualCable();

    if (deviceName.isEmpty() || deviceName == bridge.getOpenDeviceName())
        return;

    selectOutputDevice (deviceName);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StreamBridgeProcessor();
}
