// Runs the YouTube downloader without a DAW and prints its status messages.
// Usage: DownloadTest.exe <youtube link> <folder> [format number, in the menu's order: 0 = MP3 192 ... 5 = WAV]

#include "../Source/AudioDownloader.h"

#include <cstdio>
#include <cstdlib>

int main (int argc, char* argv[])
{
    if (argc < 3)
    {
        std::printf ("Usage: DownloadTest.exe <youtube link> <folder> [format number]\n");
        return 1;
    }

    const auto format = juce::jlimit (0, AudioDownloader::getFormatNames().size() - 1, argc > 3 ? std::atoi (argv[3]) : 0);
    std::printf ("Format: %s\n", AudioDownloader::getFormatNames()[format].toRawUTF8());

    AudioDownloader downloader;
    downloader.startDownload (argv[1], juce::File (argv[2]), (AudioDownloader::Format) format);

    juce::String lastPrinted;
    auto lastPrintTime = juce::Time::getMillisecondCounter();
    const auto startTime = lastPrintTime;

    do
    {
        juce::Thread::sleep (200);

        // Print when the kind of message changes, or every 3 s while it stays the same.
        const auto status = downloader.getStatus();
        const auto now = juce::Time::getMillisecondCounter();

        if (status != lastPrinted && (status.substring (0, 20) != lastPrinted.substring (0, 20) || now - lastPrintTime > 3000))
        {
            std::printf ("[%5.1f s] %s\n", (now - startTime) / 1000.0, status.toRawUTF8());
            lastPrinted = status;
            lastPrintTime = now;
        }
    }
    while (downloader.isBusy());

    const auto finalStatus = downloader.getStatus();

    if (finalStatus != lastPrinted)
        std::printf ("[%5.1f s] %s\n", (juce::Time::getMillisecondCounter() - startTime) / 1000.0, finalStatus.toRawUTF8());

    return finalStatus.startsWith ("Saved:") ? 0 : 1;
}
