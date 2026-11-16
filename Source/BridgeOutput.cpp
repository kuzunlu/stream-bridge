#include "BridgeOutput.h"

BridgeOutput::BridgeOutput()
    : deviceType (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency))
{
    if (deviceType != nullptr)
        deviceType->addListener (this);
}

BridgeOutput::~BridgeOutput()
{
    onReconnectNeeded = nullptr;
    closeDevice();

    if (deviceType != nullptr)
        deviceType->removeListener (this);
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

    // Windows' low-latency mode gives smaller buffers when the driver supports it.
    // If the device cannot open that way, use the normal mode.
    auto error = tryOpen (*deviceType, deviceName, true);

    if (error.isNotEmpty())
    {
        if (normalDeviceType == nullptr)
            normalDeviceType.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));

        if (normalDeviceType != nullptr)
            error = tryOpen (*normalDeviceType, deviceName, false);
    }

    if (error.isNotEmpty())
        return error;

    openDeviceName = deviceName;
    device->start (this);
    return {};
}

juce::String BridgeOutput::tryOpen (juce::AudioIODeviceType& type, const juce::String& deviceName, bool lowLatency)
{
    type.scanForDevices();

    if (! type.getDeviceNames (false).contains (deviceName))
        return "Device not found: " + deviceName;

    device.reset (type.createDevice (deviceName, {}));

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

    // In low-latency mode the smallest size the driver allows, otherwise Windows' default.
    const auto sizes = device->getAvailableBufferSizes();
    const auto bufferSize = lowLatency && ! sizes.isEmpty() ? sizes.getFirst() : device->getDefaultBufferSize();

    const auto error = device->open ({}, outputChannels, rate, bufferSize);

    if (error.isNotEmpty())
    {
        device.reset();
        return error;
    }

    lowLatencyActive.store (lowLatency && bufferSize < device->getDefaultBufferSize());
    return {};
}

void BridgeOutput::closeDevice()
{
    if (device != nullptr)
    {
        const juce::ScopedValueSetter<bool> purpose (closingOnPurpose, true);
        device->stop();
        device->close();
        device.reset();
    }

    running.store (false);
    openDeviceName = {};
}

bool BridgeOutput::couldUseHostSampleRate() const
{
    const auto hostRate = hostSampleRate.load();

    return device != nullptr && running.load() && hostRate != deviceSampleRate.load()
        && device->getAvailableSampleRates().contains (hostRate);
}

void BridgeOutput::push (const juce::AudioBuffer<float>& buffer)
{
    const auto numChannels = buffer.getNumChannels();

    if (! running.load() || ! enabled.load() || numChannels == 0)
        return;

    lastPushTime.store (juce::Time::getMillisecondCounter());

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

void BridgeOutput::resetHealth()
{
    dropouts.store (0);
    recentLowestMargin.store (INT_MAX);
    sessionLowestMargin.store (INT_MAX);
    lastDropoutTime.store (0);
}

std::optional<double> BridgeOutput::takeLowestMarginMs()
{
    const auto margin = recentLowestMargin.exchange (INT_MAX);

    if (margin == INT_MAX)
        return {};

    return margin * 1000.0 / hostSampleRate.load();
}

std::optional<double> BridgeOutput::getSessionLowestMarginMs() const
{
    const auto margin = sessionLowestMargin.load();

    if (margin == INT_MAX)
        return {};

    return margin * 1000.0 / hostSampleRate.load();
}

std::optional<double> BridgeOutput::getSecondsSinceLastDropout() const
{
    const auto time = lastDropoutTime.load();

    if (time == 0)
        return {};

    return (juce::Time::getMillisecondCounter() - time) / 1000.0;
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

    const auto hostRate = hostSampleRate.load();
    const auto baseRatio = hostRate / deviceSampleRate.load();

    // The smallest buffer that can work: one block of this device, one host block,
    // and a little room for timing jitter. The user's setting can only go above it.
    const auto minimumTarget = numSamples * baseRatio + hostBlockSize.load() + jitterMarginSamples;
    const auto target = juce::jlimit (minimumTarget, juce::jmax (minimumTarget, maxTargetSamples), targetLatencyMs.load() * 0.001 * hostRate);
    minimumMs.store (minimumTarget * 1000.0 / hostRate);
    targetMs.store (target * 1000.0 / hostRate);

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
        lastTarget = target;

        for (auto& interpolator : interpolators)
            interpolator.reset();
    }

    // The buffer setting was changed: go to the new amount right away, instead of
    // drifting there over many seconds. Viewers may hear one small click.
    if (target != lastTarget)
    {
        lastTarget = target;
        resetHealth();    // the old numbers belonged to the old buffer size

        if (ready < target)
        {
            priming = true;
            return;
        }

        const auto skip = ready - (int) target;
        fifo.finishedRead (skip);
        ready -= skip;
        smoothedFill = ready;
    }

    // Far too much audio waiting (for example after a CPU spike): jump forward.
    if (ready > juce::jmin (4.0 * target, fifoSize - 4096.0))
    {
        const auto skip = ready - (int) target;
        fifo.finishedRead (skip);
        ready -= skip;
        smoothedFill = ready;
    }

    // Play a little faster when the FIFO is fuller than the target, a little slower
    // when it is emptier. This cancels the clock drift between the two devices.
    smoothedFill += 0.01 * (ready - smoothedFill);
    averageFillSamples.store (smoothedFill);
    const auto fillError = juce::jlimit (-1.0, 1.0, (smoothedFill - target) / target);
    const auto ratio = baseRatio * (1.0 + maxSpeedCorrection * fillError);
    const auto needed = (int) std::ceil (ratio * numSamples) + 4;

    // Running out is a dropout only while the host is still sending. When it stops
    // (export, audio engine off, plugin switched off) the FIFO empties as expected.
    const auto now = juce::Time::getMillisecondCounter();
    const auto hostIsSending = now - lastPushTime.load() < 250;

    // How much spare audio is left after this read.
    if (hostIsSending)
    {
        const auto margin = ready - needed;

        if (margin < recentLowestMargin.load())
            recentLowestMargin.store (margin);

        if (margin < sessionLowestMargin.load())
            sessionLowestMargin.store (margin);
    }

    if (needed > ready || needed > scratch.getNumSamples() || numSamples > rendered.getNumSamples())
    {
        if (hostIsSending)
        {
            dropouts.fetch_add (1);
            lastDropoutTime.store (juce::jmax ((juce::uint32) 1, now));
        }

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
    windowsOutputMs.store (startingDevice->getOutputLatencyInSamples() * 1000.0 / juce::jmax (1.0, deviceSampleRate.load()));

    const auto blockSize = startingDevice->getCurrentBufferSizeSamples();
    scratch.setSize (2, blockSize * 8 + 64);    // room for up to an 8x sample rate difference
    rendered.setSize (2, blockSize * 2);

    smoothedGain.reset (deviceSampleRate.load(), 0.05);
    smoothedGain.setCurrentAndTargetValue (gain.load());

    // Throw away any old audio left over from an earlier session.
    fifo.finishedRead (fifo.getNumReady());
    priming = true;
    resetHealth();

    {
        const juce::ScopedLock sl (errorLock);
        lastError = {};
    }

    startTime = juce::Time::getMillisecondCounter();
    running.store (true);
}

void BridgeOutput::audioDeviceStopped()
{
    running.store (false);

    // Windows stopped the device. Ask to open it again, unless it had only just started,
    // so a device that keeps failing cannot cause an endless loop. A later device list
    // change still gets another try.
    if (! closingOnPurpose && juce::Time::getMillisecondCounter() - startTime > 1000 && onReconnectNeeded != nullptr)
        onReconnectNeeded();
}

void BridgeOutput::audioDeviceError (const juce::String& errorMessage)
{
    const juce::ScopedLock sl (errorLock);
    lastError = errorMessage;
}

void BridgeOutput::audioDeviceListChanged()
{
    // A device was added or removed. If ours was missing, it may be back now.
    if (! running.load() && onReconnectNeeded != nullptr)
        onReconnectNeeded();
}
