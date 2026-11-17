#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "WindowsAudioCheck.h"

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
                                                          juce::NormalisableRange<float> (0.0f, 250.0f, 1.0f, 0.5f), 20.0f,
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
    state.setProperty ("downloadFormatId", AudioDownloader::getFormatId (getDownloadFormat()), nullptr);

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

        downloadFormat = AudioDownloader::getFormatFromId (state.getProperty ("downloadFormatId").toString());
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

    {
        const juce::ScopedLock sl (deviceLock);
        openError = error;
    }

    checkWindowsSettings();
}

void StreamBridgeProcessor::checkWindowsSettings()
{
    const auto device = getSelectedOutputDevice();
    juce::String warning;

    if (device.isNotEmpty())
    {
        if (const auto listening = WindowsAudioCheck::findListeningRecordingDevice (device); listening.isNotEmpty())
            warning = "You hear yourself twice: Windows plays " + listening + " to your speakers. Fix: Win+R, mmsys.cpl, "
                      "Recording, " + listening + ", Properties, Listen, untick \"Listen to this device\".";
        else if (! WindowsAudioCheck::looksVirtual (device))
            warning = "This looks like a real speaker or headphone output, so you will hear the copy late. "
                      "Pick a virtual cable, like CABLE Input.";
        else if (const auto deviceRate = juce::roundToInt (bridge.getDeviceSampleRate()), hostRate = juce::roundToInt (getSampleRate());
                 bridge.isRunning() && deviceRate > 0 && hostRate > 0 && deviceRate != hostRate)
            warning = "The output runs at " + juce::String (deviceRate) + " Hz but your DAW at " + juce::String (hostRate)
                      + " Hz, so the copy is converted and loses some treble. Fix: Win+R, mmsys.cpl, Playback, " + device
                      + ", Properties, Advanced, pick a " + juce::String (hostRate) + " Hz format, OK. Then click Refresh.";
    }

    const juce::ScopedLock sl (deviceLock);
    warningText = warning;
}

juce::String StreamBridgeProcessor::getWarningText() const
{
    const juce::ScopedLock sl (deviceLock);
    return warningText;
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

AudioDownloader::Format StreamBridgeProcessor::getDownloadFormat() const
{
    const juce::ScopedLock sl (deviceLock);
    return downloadFormat;
}

void StreamBridgeProcessor::setDownloadFormat (AudioDownloader::Format format)
{
    const juce::ScopedLock sl (deviceLock);
    downloadFormat = format;
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
    {
        checkWindowsSettings();    // the host's rate may have changed, which can change the warning
        return;
    }

    selectOutputDevice (deviceName);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StreamBridgeProcessor();
}
