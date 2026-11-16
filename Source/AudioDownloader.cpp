#include "AudioDownloader.h"

#include <juce_cryptography/juce_cryptography.h>

// Pinned tool versions. The folder name holds the version, so changing a version here
// makes the plugin download the new one next time.
struct AudioDownloader::Tool
{
    const char* name;
    const char* folder;
    const char* url;
    const char* sha256;
    bool isZip;
    const char* files[2];
};

namespace
{
    constexpr int ytDlp = 0, ffmpeg = 1, deno = 2;

    const AudioDownloader::Tool tools[] =
    {
        { "yt-dlp", "yt-dlp-2026.10.26",
          "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe",
          "f5389d3698ecde4ef06f4e6b1ce0294fe9ac1396228c8990b8d3416d6d177a7d",
          false, { "yt-dlp.exe", nullptr } },

        { "ffmpeg", "ffmpeg-9.0.2",
          "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
          "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
          true, { "ffmpeg.exe", "ffprobe.exe" } },

        { "Deno", "deno-2.9.8",
          "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip",
          "cba84b26e0df5ae7f74fc66b86a95b866872a86292ebd84fa1e959927edd93bf",
          true, { "deno.exe", nullptr } },
    };

    juce::String megabytes (juce::int64 bytes)
    {
        return juce::String (juce::roundToInt ((double) bytes / (1024.0 * 1024.0))) + " MB";
    }
}

juce::StringArray AudioDownloader::getFormatNames()
{
    return { "MP3 320 kbps (best)", "MP3 256 kbps", "MP3 192 kbps", "MP3 128 kbps", "WAV (lossless, large file)" };
}

AudioDownloader::AudioDownloader()
    : juce::Thread ("Stream Bridge downloader"),
      toolsFolder (juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
                       .getChildFile ("Stream Bridge").getChildFile ("tools"))
{
}

AudioDownloader::~AudioDownloader()
{
    cancel();
    stopThread (10000);
}

void AudioDownloader::startDownload (const juce::String& url, const juce::File& folder, Format format)
{
    if (isThreadRunning())
        return;

    if (url.trim().isEmpty())
    {
        setStatus ("Paste a YouTube link first.");
        return;
    }

    if (! folder.isDirectory())
    {
        setStatus ("The save folder does not exist. Choose another one.");
        return;
    }

    job = Job::download;
    jobUrl = url.trim();
    jobFolder = folder;
    jobFormat = format;
    cancelled.store (false);
    progress.store (0.0);
    setStatus ("Starting...");
    startThread();
}

void AudioDownloader::startUpdate()
{
    if (isThreadRunning())
        return;

    job = Job::update;
    cancelled.store (false);
    progress.store (-1.0);
    setStatus ("Updating yt-dlp...");
    startThread();
}

void AudioDownloader::cancel()
{
    cancelled.store (true);
    signalThreadShouldExit();

    const juce::ScopedLock sl (processLock);

    if (process != nullptr)
        process->kill();
}

juce::String AudioDownloader::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

void AudioDownloader::setStatus (const juce::String& newStatus)
{
    const juce::ScopedLock sl (statusLock);
    status = newStatus;
}

juce::File AudioDownloader::getToolFile (const Tool& tool, const char* fileName) const
{
    return toolsFolder.getChildFile (tool.folder).getChildFile (fileName);
}

void AudioDownloader::run()
{
    for (int i = 0; i < 3; ++i)
    {
        if (! installTool (tools[i]))
        {
            if (cancelled.load())
                setStatus ("Cancelled.");

            return;
        }
    }

    savedFile = {};
    lastError = {};
    lastLine = {};

    if (job == Job::update)
    {
        const auto exitCode = runYtDlp ({ "-U" });
        setStatus (cancelled.load() ? "Cancelled." : exitCode == 0 ? lastLine : "Update failed: " + lastLine);
        progress.store (exitCode == 0 ? 1.0 : 0.0);
        return;
    }

    const auto isWav = jobFormat == Format::wav;
    const char* bitrates[] = { "320K", "256K", "192K", "128K" };

    juce::StringArray arguments { "--no-playlist", "--newline", "--progress", "--encoding", "utf-8",
                                  "--ffmpeg-location", toolsFolder.getChildFile (tools[ffmpeg].folder).getFullPathName(),
                                  "--no-js-runtimes", "--js-runtimes", "deno:" + getToolFile (tools[deno], "deno.exe").getFullPathName(),
                                  "-f", "bestaudio/best", "-x", "--audio-format", isWav ? "wav" : "mp3" };

    if (! isWav)
        arguments.addArray ({ "--audio-quality", bitrates[(int) jobFormat] });

    arguments.addArray ({ "-o", jobFolder.getChildFile ("%(title)s.%(ext)s").getFullPathName(),
                          "--print", "after_move:SAVED|%(filepath)s",
                          "--", jobUrl });

    setStatus ("Reading the YouTube page...");
    progress.store (-1.0);

    const auto exitCode = runYtDlp (arguments);

    if (cancelled.load())
    {
        setStatus ("Cancelled.");
        progress.store (0.0);
    }
    else if (exitCode == 0 && savedFile.isNotEmpty())
    {
        setStatus ("Saved: " + juce::File (savedFile).getFileName());
        progress.store (1.0);
    }
    else
    {
        setStatus (lastError.isNotEmpty() ? lastError : "Download failed: " + lastLine);
        progress.store (0.0);
    }
}

bool AudioDownloader::installTool (const Tool& tool)
{
    const auto isInstalled = [&]
    {
        for (const auto* fileName : tool.files)
            if (fileName != nullptr && ! getToolFile (tool, fileName).existsAsFile())
                return false;

        return true;
    };

    if (isInstalled())
        return true;

    const auto folder = toolsFolder.getChildFile (tool.folder);

    if (! folder.createDirectory())
    {
        setStatus ("Could not create " + folder.getFullPathName());
        return false;
    }

    const auto download = folder.getChildFile ("download.tmp");

    if (! downloadFile (tool.url, download, tool.name))
    {
        download.deleteFile();
        return false;
    }

    setStatus ("Checking " + juce::String (tool.name) + "...");
    progress.store (-1.0);

    if (! juce::SHA256 (download).toHexString().equalsIgnoreCase (tool.sha256))
    {
        download.deleteFile();
        setStatus ("The downloaded " + juce::String (tool.name) + " did not match its checksum, so it was not used. Try again.");
        return false;
    }

    if (! tool.isZip)
        return download.moveFileTo (getToolFile (tool, tool.files[0]));

    setStatus ("Unpacking " + juce::String (tool.name) + "...");

    {
        juce::ZipFile zip (download);

        for (const auto* fileName : tool.files)
        {
            if (fileName == nullptr)
                continue;

            int entryIndex = -1;

            for (int i = 0; i < zip.getNumEntries(); ++i)
            {
                const auto entryName = zip.getEntry (i)->filename.replaceCharacter ('\\', '/');

                if (entryName == fileName || entryName.endsWith (juce::String ("/") + fileName))
                    entryIndex = i;
            }

            const auto target = getToolFile (tool, fileName);
            const auto part = target.withFileExtension (".part");
            const std::unique_ptr<juce::InputStream> in (entryIndex >= 0 ? zip.createStreamForEntry (entryIndex) : nullptr);

            if (in == nullptr)
            {
                setStatus (juce::String (fileName) + " was not found in the " + tool.name + " download.");
                return false;
            }

            part.deleteFile();

            {
                juce::FileOutputStream out (part);

                if (out.failedToOpen() || out.writeFromInputStream (*in, -1) <= 0)
                {
                    setStatus ("Could not unpack " + juce::String (fileName));
                    return false;
                }
            }

            part.moveFileTo (target);
        }
    }

    download.deleteFile();
    return isInstalled();
}

bool AudioDownloader::downloadFile (const juce::String& url, const juce::File& target, const juce::String& toolName)
{
    setStatus ("Connecting to download " + toolName + "...");
    progress.store (-1.0);

    const auto stream = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                               .withConnectionTimeoutMs (20000)
                                                               .withNumRedirectsToFollow (5));

    if (stream == nullptr)
    {
        setStatus ("Could not connect to download " + toolName + ". Check your internet connection.");
        return false;
    }

    target.deleteFile();
    juce::FileOutputStream out (target);

    if (out.failedToOpen())
    {
        setStatus ("Could not write " + target.getFullPathName());
        return false;
    }

    const auto total = stream->getTotalLength();
    juce::HeapBlock<char> buffer (1 << 16);
    juce::int64 done = 0;

    while (! stream->isExhausted())
    {
        if (threadShouldExit())
            return false;

        const auto numRead = stream->read (buffer, 1 << 16);

        if (numRead <= 0)
            break;

        out.write (buffer, (size_t) numRead);
        done += numRead;

        progress.store (total > 0 ? (double) done / (double) total : -1.0);
        setStatus ("First time only: downloading " + toolName + " (" + megabytes (done)
                   + (total > 0 ? " of " + megabytes (total) : juce::String()) + ")");
    }

    out.flush();
    return total <= 0 || done == total;
}

int AudioDownloader::runYtDlp (const juce::StringArray& arguments)
{
    juce::StringArray command { getToolFile (tools[ytDlp], "yt-dlp.exe").getFullPathName() };
    command.addArray (arguments);

    {
        const juce::ScopedLock sl (processLock);
        process = std::make_unique<juce::ChildProcess>();

        if (cancelled.load() || ! process->start (command))
        {
            process.reset();
            lastLine = "Could not start yt-dlp.";
            return -1;
        }
    }

    // yt-dlp writes one line per progress update (--newline), so read line by line.
    juce::MemoryOutputStream line;
    char c = 0;

    const auto flushLine = [&]
    {
        if (line.getDataSize() > 0)
            handleOutputLine (juce::String::fromUTF8 ((const char*) line.getData(), (int) line.getDataSize()).trim());

        line.reset();
    };

    while (process->readProcessOutput (&c, 1) == 1)
    {
        if (c == '\n' || c == '\r')
            flushLine();
        else
            line.writeByte (c);
    }

    flushLine();
    process->waitForProcessToFinish (5000);

    const juce::ScopedLock sl (processLock);
    const auto exitCode = (int) process->getExitCode();
    process.reset();
    return exitCode;
}

void AudioDownloader::handleOutputLine (const juce::String& line)
{
    if (line.isEmpty())
        return;

    if (line.startsWith ("SAVED|"))
    {
        savedFile = line.fromFirstOccurrenceOf ("|", false, false);
        return;
    }

    lastLine = line;

    if (line.startsWith ("ERROR:"))
    {
        lastError = line;
    }
    else if (line.startsWith ("[download]") && line.containsChar ('%'))
    {
        const auto percent = line.fromFirstOccurrenceOf ("]", false, false).trim().upToFirstOccurrenceOf ("%", false, false).getDoubleValue();
        progress.store (juce::jlimit (0.0, 1.0, percent / 100.0));
        setStatus ("Downloading audio: " + juce::String (juce::roundToInt (percent)) + "%");
    }
    else if (line.startsWith ("[ExtractAudio]"))
    {
        progress.store (-1.0);
        setStatus (jobFormat == Format::wav ? "Converting to WAV..." : "Converting to MP3...");
    }
    else if (job == Job::update)
    {
        setStatus (line);
    }
}
