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
    constexpr int ytDlp = 0, ffmpeg = 1, quickJs = 2;

    const AudioDownloader::Tool tools[] =
    {
        { "yt-dlp", "yt-dlp-2026.10.26",
          "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe",
          "f5389d3698ecde4ef06f4e6b1ce0294fe9ac1396228c8990b8d3416d6d177a7d",
          false, { "yt-dlp.exe", nullptr } },

        { "ffmpeg", "ffmpeg-9.0.2",
          "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
          "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
          true, { "ffmpeg.exe", nullptr } },

        // A small JavaScript runtime. yt-dlp needs one to read YouTube's pages, and it
        // finds QuickJS by the file name qjs.exe.
        { "QuickJS", "quickjs-ng-0.18.0",
          "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-windows-x86_64.exe",
          "ff03398d74ea6e6bd3a8e58058f7003225abcac229d7d57775b7aaae812a49c7",
          false, { "qjs.exe", nullptr } },
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

void AudioDownloader::removeOldTools()
{
    for (const auto& folder : toolsFolder.findChildFiles (juce::File::findDirectories, false))
    {
        const Tool* match = nullptr;

        for (const auto& tool : tools)
            if (folder.getFileName() == tool.folder)
                match = &tool;

        if (match == nullptr)
        {
            folder.deleteRecursively();
            continue;
        }

        for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
            if (file.getFileName() != match->files[0] && file.getFileName() != juce::String (match->files[1]))
                file.deleteFile();
    }
}

void AudioDownloader::run()
{
    removeOldTools();

    for (const auto& tool : tools)
    {
        if (! installTool (tool))
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
        const auto exitCode = runTool (tools[ytDlp], { "-U" });
        setStatus (cancelled.load() ? "Cancelled." : exitCode == 0 ? lastLine : "Update failed: " + lastLine);
        progress.store (exitCode == 0 ? 1.0 : 0.0);
        return;
    }

    // Step 1: yt-dlp downloads the best audio stream, unchanged, into a private work folder.
    const auto workFolder = toolsFolder.getSiblingFile ("work").getChildFile (juce::Uuid().toString());
    workFolder.createDirectory();

    juce::StringArray arguments { "--no-playlist", "--newline", "--progress", "--encoding", "utf-8",
                                  "--ffmpeg-location", toolsFolder.getChildFile (tools[ffmpeg].folder).getFullPathName(),
                                  "--no-js-runtimes", "--js-runtimes", "quickjs:" + getToolFile (tools[quickJs], "qjs.exe").getFullPathName(),
                                  "-f", "bestaudio/best",
                                  "-o", workFolder.getChildFile ("%(title)s.%(ext)s").getFullPathName(),
                                  "--print", "after_move:SAVED|%(filepath)s",
                                  "--", jobUrl };

    setStatus ("Reading the YouTube page...");
    progress.store (-1.0);

    const auto downloadExitCode = runTool (tools[ytDlp], arguments);

    if (cancelled.load() || downloadExitCode != 0 || savedFile.isEmpty())
    {
        workFolder.deleteRecursively();
        setStatus (cancelled.load()            ? "Cancelled."
                   : lastError.isNotEmpty()    ? lastError
                                               : "Download failed: " + lastLine);
        progress.store (0.0);
        return;
    }

    // Step 2: ffmpeg converts it into the save folder, with the same settings yt-dlp uses.
    const auto isWav = jobFormat == Format::wav;
    const char* bitrates[] = { "320k", "256k", "192k", "128k" };
    const juce::File downloaded (savedFile);
    const auto target = jobFolder.getChildFile (downloaded.getFileNameWithoutExtension() + (isWav ? ".wav" : ".mp3"));

    juce::StringArray conversion { "-hide_banner", "-nostdin", "-y", "-i", downloaded.getFullPathName(), "-vn" };

    if (isWav)
        conversion.addArray ({ "-c:a", "pcm_s16le" });
    else
        conversion.addArray ({ "-c:a", "libmp3lame", "-b:a", bitrates[(int) jobFormat] });

    conversion.add (target.getFullPathName());

    setStatus (isWav ? "Converting to WAV..." : "Converting to MP3...");
    progress.store (-1.0);

    const auto convertExitCode = runTool (tools[ffmpeg], conversion);
    workFolder.deleteRecursively();

    if (cancelled.load())
    {
        target.deleteFile();
        setStatus ("Cancelled.");
        progress.store (0.0);
    }
    else if (convertExitCode == 0 && target.existsAsFile())
    {
        setStatus ("Saved: " + target.getFileName());
        progress.store (1.0);
    }
    else
    {
        setStatus ("Converting failed: " + lastLine);
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

int AudioDownloader::runTool (const Tool& tool, const juce::StringArray& arguments)
{
    juce::StringArray command { getToolFile (tool, tool.files[0]).getFullPathName() };
    command.addArray (arguments);

    {
        const juce::ScopedLock sl (processLock);
        process = std::make_unique<juce::ChildProcess>();

        if (cancelled.load() || ! process->start (command))
        {
            process.reset();
            lastLine = "Could not start " + juce::String (tool.name) + ".";
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
