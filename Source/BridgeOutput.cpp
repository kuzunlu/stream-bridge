#include "BridgeOutput.h"

BridgeOutput::BridgeOutput()
    : deviceType (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared))
{
}

BridgeOutput::~BridgeOutput()
{
    closeDevice();
}

juce::StringArray BridgeOutput::getOutputDeviceNames()
{
    if (deviceType == nullptr)
        return {};

    deviceType->scanForDevices();
    return deviceType->getDeviceNames (false);
}

juce::String BridgeOutput::openDevice (const juce::String& deviceName)
{
    closeDevice();

    if (deviceType == nullptr)
        return "Windows Audio is not available";

    deviceType->scanForDevices();

    if (! deviceType->getDeviceNames (false).contains (deviceName))
        return "Device not found: " + deviceName;

    device.reset (deviceType->createDevice (deviceName, {}));

    if (device == nullptr)
        return "Could not create device: " + deviceName;

    // Use the host's rate if the device allows it, so no real resampling is needed.
    const auto rates = device->getAvailableSampleRates();
    const auto hostRate = hostSampleRate.load();
    const auto rate = rates.contains (hostRate) ? hostRate
                    : rates.contains (48000.0)  ? 48000.0
                    : rates.isEmpty()           ? 0.0
                                                : rates.getFirst();

    juce::BigInteger outputChannels;
    outputChannels.setRange (0, juce::jmin (2, device->getOutputChannelNames().size()), true);

    const auto error = device->open ({}, outputChannels, rate, device->getDefaultBufferSize());

    if (error.isNotEmpty())
    {
        device.reset();
        return error;
    }

    openDeviceName = deviceName;
    device->start (this);
    return {};
}

void BridgeOutput::closeDevice()
{
    if (device != nullptr)
    {
        device->stop();
        device->close();
        device.reset();
    }

    running.store (false);
    openDeviceName = {};
}

void BridgeOutput::push (const juce::AudioBuffer<float>& buffer)
{
    const auto numChannels = buffer.getNumChannels();

    if (! running.load() || ! enabled.load() || numChannels == 0)
        return;

    // If the FIFO is full, the extra samples are dropped. The output side keeps the
    // FIFO far from full, so this only happens if the output device stalls.
    const auto scope = fifo.write (buffer.getNumSamples());

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto source = juce::jmin (ch, numChannels - 1);

        if (scope.blockSize1 > 0)
            fifoBuffer.copyFrom (ch, scope.startIndex1, buffer, source, 0, scope.blockSize1);

        if (scope.blockSize2 > 0)
            fifoBuffer.copyFrom (ch, scope.startIndex2, buffer, source, scope.blockSize1, scope.blockSize2);
    }
}

double BridgeOutput::getBufferedMs() const
{
    return fifo.getNumReady() * 1000.0 / hostSampleRate.load();
}

juce::String BridgeOutput::getLastError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastError;
}

void BridgeOutput::audioDeviceIOCallbackWithContext (const float* const*,
                                                     int,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    const auto baseRatio = hostSampleRate.load() / deviceSampleRate.load();
    // The host delivers audio in blocks, so the FIFO must hold more than one host block.
    const auto target = juce::jmax (hostSampleRate.load() * targetLatencySeconds,
                                    2.0 * numSamples * baseRatio,
                                    1.5 * hostBlockSize.load() + numSamples * baseRatio);
    auto ready = fifo.getNumReady();

    if (! enabled.load())
    {
        fifo.finishedRead (ready);
        priming = true;
        return;
    }

    // After a start or a dropout, wait until the FIFO holds the target amount again.
    if (priming)
    {
        if (ready < target)
            return;

        priming = false;
        smoothedFill = ready;

        for (auto& interpolator : interpolators)
            interpolator.reset();
    }

    // Far too much audio waiting (for example after a CPU spike): jump forward.
    if (ready > 4.0 * target)
    {
        const auto skip = ready - (int) target;
        fifo.finishedRead (skip);
        ready -= skip;
        smoothedFill = ready;
    }

    // Play a little faster when the FIFO is fuller than the target, a little slower
    // when it is emptier. This cancels the clock drift between the two devices.
    smoothedFill += 0.01 * (ready - smoothedFill);
    const auto fillError = juce::jlimit (-1.0, 1.0, (smoothedFill - target) / target);
    const auto ratio = baseRatio * (1.0 + maxSpeedCorrection * fillError);
    const auto needed = (int) std::ceil (ratio * numSamples) + 4;

    if (needed > ready || needed > scratch.getNumSamples() || numSamples > rendered.getNumSamples())
    {
        dropouts.fetch_add (1);
        priming = true;
        return;
    }

    int start1, size1, start2, size2;
    fifo.prepareToRead (needed, start1, size1, start2, size2);

    for (int ch = 0; ch < 2; ++ch)
    {
        scratch.copyFrom (ch, 0, fifoBuffer, ch, start1, size1);

        if (size2 > 0)
            scratch.copyFrom (ch, size1, fifoBuffer, ch, start2, size2);
    }

    int used = 0;

    for (int ch = 0; ch < 2; ++ch)
        used = interpolators[(size_t) ch].process (ratio, scratch.getReadPointer (ch), rendered.getWritePointer (ch), numSamples);

    fifo.finishedRead (used);

    smoothedGain.setTargetValue (gain.load());
    smoothedGain.applyGain (rendered, numSamples);

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::copy (outputChannelData[ch], rendered.getReadPointer (juce::jmin (ch, 1)), numSamples);
}

void BridgeOutput::audioDeviceAboutToStart (juce::AudioIODevice* startingDevice)
{
    deviceSampleRate.store (startingDevice->getCurrentSampleRate());

    const auto blockSize = startingDevice->getCurrentBufferSizeSamples();
    scratch.setSize (2, blockSize * 8 + 64);    // room for up to an 8x sample rate difference
    rendered.setSize (2, blockSize * 2);

    smoothedGain.reset (deviceSampleRate.load(), 0.05);
    smoothedGain.setCurrentAndTargetValue (gain.load());

    // Throw away any old audio left over from an earlier session.
    fifo.finishedRead (fifo.getNumReady());
    priming = true;
    dropouts.store (0);

    {
        const juce::ScopedLock sl (errorLock);
        lastError = {};
    }

    running.store (true);
}

void BridgeOutput::audioDeviceStopped()
{
    running.store (false);
}

void BridgeOutput::audioDeviceError (const juce::String& errorMessage)
{
    const juce::ScopedLock sl (errorLock);
    lastError = errorMessage;
}
