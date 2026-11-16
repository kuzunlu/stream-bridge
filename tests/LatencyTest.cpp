// Runs the plugin the way Ableton does (48000 Hz, 128-sample blocks, one block every
// 2.67 ms) and checks that:
//   - the plugin reports 0 latency,
//   - the output of every block is exactly the same as its input (0 samples of delay),
//   - how much of the 2.67 ms block budget the plugin uses,
//   - how much CPU and memory the plugin uses.

#include "../Source/PluginProcessor.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#pragma comment (lib, "winmm.lib")

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 128;
    constexpr int numNoiseBlocks = 64;

    struct Result
    {
        std::vector<double> microseconds;
        bool outputMatchesInput = true;
        double cpuPercentOfOneCore = 0.0;
    };

    double getProcessCpuSeconds()
    {
        FILETIME created, exited, kernel, user;
        GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user);

        const auto toSeconds = [] (FILETIME t) { return (double) ((ULONGLONG) t.dwHighDateTime << 32 | t.dwLowDateTime) * 1.0e-7; };
        return toSeconds (kernel) + toSeconds (user);
    }

    double getPrivateMemoryMB()
    {
        PROCESS_MEMORY_COUNTERS_EX counters {};
        GetProcessMemoryInfo (GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &counters, sizeof (counters));
        return (double) counters.PrivateUsage / (1024.0 * 1024.0);
    }

    Result runLikeAbleton (StreamBridgeProcessor& processor, const juce::AudioBuffer<float>& noise, double seconds)
    {
        juce::AudioBuffer<float> buffer (2, testBlockSize);
        juce::MidiBuffer midi;

        const auto numBlocks = (int) (seconds * testSampleRate / testBlockSize);
        const auto blockDuration = std::chrono::duration_cast<Clock::duration> (std::chrono::duration<double> (testBlockSize / testSampleRate));

        Result result;
        result.microseconds.reserve ((size_t) numBlocks);

        const auto cpuStart = getProcessCpuSeconds();
        const auto wallStart = Clock::now();
        auto nextBlock = wallStart;

        for (int i = 0; i < numBlocks; ++i)
        {
            const auto noiseOffset = (i % numNoiseBlocks) * testBlockSize;

            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, noise, ch, noiseOffset, testBlockSize);

            const auto start = Clock::now();
            processor.processBlock (buffer, midi);
            const auto end = Clock::now();

            result.microseconds.push_back (std::chrono::duration<double, std::micro> (end - start).count());

            for (int ch = 0; ch < 2; ++ch)
                if (std::memcmp (buffer.getReadPointer (ch), noise.getReadPointer (ch, noiseOffset), sizeof (float) * testBlockSize) != 0)
                    result.outputMatchesInput = false;

            // Wait for the next block, like an audio driver would.
            nextBlock += blockDuration;
            std::this_thread::sleep_until (nextBlock);
        }

        const auto wallSeconds = std::chrono::duration<double> (Clock::now() - wallStart).count();
        result.cpuPercentOfOneCore = (getProcessCpuSeconds() - cpuStart) / wallSeconds * 100.0;
        return result;
    }

    void printTimes (const char* title, std::vector<double> times)
    {
        std::sort (times.begin(), times.end());
        const auto budget = testBlockSize / testSampleRate * 1.0e6;
        const auto at = [&] (double fraction) { return times[(size_t) (fraction * (double) (times.size() - 1))]; };

        std::printf ("%s (%d blocks)\n", title, (int) times.size());
        std::printf ("  typical (median): %8.3f us  = %.4f%% of the %.0f us block budget\n", at (0.5), at (0.5) / budget * 100.0, budget);
        std::printf ("  99%% of blocks:    %8.3f us\n", at (0.99));
        std::printf ("  slowest block:    %8.3f us  (can include Windows pausing the test itself)\n", times.back());
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::Process::setPriority (juce::Process::HighPriority);
    timeBeginPeriod (1);    // lets the test wake up on time between blocks

    juce::AudioBuffer<float> noise (2, testBlockSize * numNoiseBlocks);
    juce::Random random (1234);

    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < noise.getNumSamples(); ++s)
            noise.setSample (ch, s, random.nextFloat() * 2.0f - 1.0f);

    const auto memoryBefore = getPrivateMemoryMB();

    StreamBridgeProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);
    processor.bridge.setMonitoring (true);    // tuning mode, so dropouts are counted

    // -60 dB is the lowest volume and means silence, so the cable only gets silence.
    processor.parameters.getParameter ("gain")->setValueNotifyingHost (0.0f);

    // Optional second argument: the stream buffer in ms, for example: LatencyTest.exe "Speakers (Senary Audio)" 0
    if (argc > 2)
    {
        auto* buffer = processor.parameters.getParameter ("bufferMs");
        buffer->setValueNotifyingHost (buffer->convertTo0to1 (juce::String (argv[2]).getFloatValue()));
    }

    // Pass a device name to test with another output, for example: LatencyTest.exe "Speakers (Senary Audio)"
    juce::String cable = argc > 1 ? juce::String (argv[1]) : juce::String();

    if (cable.isEmpty())
        for (const auto& name : processor.bridge.getOutputDeviceNames())
            if (name.containsIgnoreCase ("CABLE") || name.containsIgnoreCase ("Virtual"))
                cable = name;

    if (cable.isEmpty())
    {
        std::printf ("No virtual cable found. Output devices:\n");

        for (const auto& name : processor.bridge.getOutputDeviceNames())
            std::printf ("  %s\n", name.toRawUTF8());

        return 1;
    }

    processor.selectOutputDevice (cable);

    for (int i = 0; i < 300 && ! processor.bridge.isRunning(); ++i)
        juce::Thread::sleep (10);

    if (! processor.bridge.isRunning())
    {
        std::printf ("Could not start the output: %s\n", processor.getStatusText().toRawUTF8());
        return 1;
    }

    std::printf ("Output device: %s\n", cable.toRawUTF8());
    std::printf ("Latency reported to the host: %d samples\n\n", processor.getLatencySamples());

    runLikeAbleton (processor, noise, 1.0);    // warm up, so the Discord-side buffer is filled

    const auto sendOn = runLikeAbleton (processor, noise, 10.0);
    std::printf ("Discord side after 10 s: %s\n", processor.getStatusText().toRawUTF8());

    if (const auto margin = processor.bridge.getSessionLowestMarginMs())
        std::printf ("Lowest safety margin: %.1f ms\n", *margin);
    std::printf ("Memory added by the plugin (running): %.2f MB\n\n", getPrivateMemoryMB() - memoryBefore);

    processor.parameters.getParameter ("enabled")->setValueNotifyingHost (0.0f);
    const auto sendOff = runLikeAbleton (processor, noise, 5.0);

    processor.bridge.closeDevice();
    const auto idle = runLikeAbleton (processor, noise, 5.0);

    printTimes ("Plugin, Send audio ON", sendOn.microseconds);
    printTimes ("Plugin, Send audio OFF", sendOff.microseconds);
    std::printf ("\nCPU of the whole test program, as %% of one CPU core:\n");
    std::printf ("  Send audio ON, cable open:  %.3f%%\n", sendOn.cpuPercentOfOneCore);
    std::printf ("  Send audio OFF, cable open: %.3f%%\n", sendOff.cpuPercentOfOneCore);
    std::printf ("  Cable closed (test loop only, for comparison): %.3f%%\n\n", idle.cpuPercentOfOneCore);

    const auto exact = sendOn.outputMatchesInput && sendOff.outputMatchesInput;
    std::printf ("Output exactly equal to input in every block: %s\n", exact ? "YES (0 samples of delay, sound unchanged)" : "NO");

    timeEndPeriod (1);
    return exact && processor.getLatencySamples() == 0 ? 0 : 1;
}
