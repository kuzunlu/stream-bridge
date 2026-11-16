#include "AudioDownloader.h"

#include <juce_cryptography/juce_cryptography.h>

#include <iterator>

// Pinned tool versions. The folder name holds the version, so changing a version here
// makes the plugin download the new one next time. ffmpeg is not here: a small build of
// it ships inside the plugin (see ffmpeg/build.sh).
struct AudioDownloader::Tool
{
    const char* name;
    const char* folder;
    const char* url;
    const char* sha256;
    const char* file;
};

namespace
{
    constexpr int ytDlp = 0, quickJs = 1;

    const AudioDownloader::Tool tools[] =
    {
        { "yt-dlp", "yt-dlp-2026.10.26",
          "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe",
          "f5389d3698ecde4ef06f4e6b1ce0294fe9ac1396228c8990b8d3416d6d177a7d",
          "yt-dlp.exe" },

        // A small JavaScript runtime. yt-dlp needs one to read YouTube's pages, and it
        // finds QuickJS by the file name qjs.exe.
        { "QuickJS", "quickjs-ng-0.18.0",
          "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-windows-x86_64.exe",
          "ff03398d74ea6e6bd3a8e58058f7003225abcac229d7d57775b7aaae812a49c7",
          "qjs.exe" },
    };

    // Same order as AudioDownloader::Format. YouTube's audio is about 128-160 kbps, so
    // MP3 above 192 kbps only makes the file bigger.
    struct FormatSpec
    {
        const char* id;
        const char* name;
        const char* kind;
        const char* extension;
        const char* ffmpegArguments;
    };

    const FormatSpec formats[] =
    {
        { "mp3_192", "MP3 192 kbps (recommended)",   "MP3",  ".mp3",  "-c:a libmp3lame -b:a 192k" },
        { "mp3_128", "MP3 128 kbps (smaller file)",  "MP3",  ".mp3",  "-c:a libmp3lame -b:a 128k" },
        { "mp3_256", "MP3 256 kbps",                 "MP3",  ".mp3",  "-c:a libmp3lame -b:a 256k" },
        { "mp3_320", "MP3 320 kbps (larger file)",   "MP3",  ".mp3",  "-c:a libmp3lame -b:a 320k" },
        { "flac",    "FLAC (lossless)",              "FLAC", ".flac", "-c:a flac -sample_fmt s16" },
        { "wav",     "WAV (lossless, largest file)", "WAV",  ".wav",  "-c:a pcm_s16le" },
    };

    // In the plugin this is Stream Bridge.vst3/Contents/Resources/ffmpeg.exe. The test
    // programs have it next to their .exe.
    juce::File findFfmpeg()
    {
        const auto module = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

        for (const auto& file : { module.getParentDirectory().getSiblingFile ("Resources").getChildFile ("ffmpeg.exe"),
                                  module.getSiblingFile ("ffmpeg.exe") })
            if (file.existsAsFile())
                return file;

        return {};
    }

    juce::String megabytes (juce::int64 bytes)
    {
        return juce::String (juce::roundToInt ((double) bytes / (1024.0 * 1024.0))) + " MB";
    }

    // "00:03:21.45" to seconds, or 0 if it is not a time (for example "N/A").
    double parseTime (const juce::String& text)
    {
        const auto parts = juce::StringArray::fromTokens (text.trim(), ":", "");

        if (parts.size() != 3)
            return 0.0;

        return parts[0].getIntValue() * 3600.0 + parts[1].getIntValue() * 60.0 + parts[2].getDoubleValue();
    }
}

juce::StringArray AudioDownloader::getFormatNames()
{
    juce::StringArray names;

    for (const auto& format : formats)
        names.add (format.name);

    return names;
}

juce::String AudioDownloader::getFormatId (Format format)
{
    return formats[(size_t) format].id;
}

AudioDownloader::Format AudioDownloader::getFormatFromId (const juce::String& id)
{
    for (size_t i = 0; i < std::size (formats); ++i)
        if (id == formats[i].id)
            return (Format) i;

    return Format::mp3_192;
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

    {
        const juce::ScopedLock sl (statusLock);
        savedFile = juce::File();
    }

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

    if (webStream != nullptr)
        webStream->cancel();
}

juce::String AudioDownloader::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

juce::File AudioDownloader::getSavedFile() const
{
    const juce::ScopedLock sl (statusLock);
    return savedFile;
}

void AudioDownloader::setStatus (const juce::String& newStatus)
{
    const juce::ScopedLock sl (statusLock);
    status = newStatus;
}

void AudioDownloader::finish (const juce::String& newStatus, double newProgress)
{
    setStatus (cancelled.load() ? juce::String ("Cancelled.") : newStatus);
    progress.store (cancelled.load() ? 0.0 : newProgress);
}

juce::File AudioDownloader::getToolFile (const Tool& tool) const
{
    return toolsFolder.getChildFile (tool.folder).getChildFile (tool.file);
}

void AudioDownloader::run()
{
    const auto ffmpeg = findFfmpeg();

    if (job == Job::download && ! ffmpeg.existsAsFile())
    {
        finish ("ffmpeg.exe is missing from the plugin folder. Install Stream Bridge again.", 0.0);
        return;
    }

    if (! installTools())
        return;

    if (job == Job::update)
    {
        const auto updated = updateYtDlp();
        finish (lastLine.isNotEmpty() ? lastLine : juce::String ("Update failed."), updated || lastLine.contains ("up to date") ? 1.0 : 0.0);
        return;
    }

    removeOldWorkFolders();

    juce::String error;

    if (downloadAudio (ffmpeg, error))
        return;

    // yt-dlp failed. YouTube often changes in ways that break older yt-dlp versions,
    // so get the newest one and try once more. If there is no newer one, show the error.
    if (! cancelled.load())
    {
        setStatus ("Download failed. Looking for a newer yt-dlp...");
        progress.store (-1.0);

        if (updateYtDlp() && ! cancelled.load())
        {
            setStatus ("yt-dlp was updated. Trying again...");

            if (downloadAudio (ffmpeg, error))
                return;
        }
    }

    finish (error, 0.0);
}

bool AudioDownloader::lockTools()
{
    if (toolsLock.enter (0))
        return true;

    setStatus ("Waiting for another Stream Bridge to finish...");
    progress.store (-1.0);

    while (! toolsLock.enter (200))
        if (threadShouldExit())
            return false;

    return true;
}

bool AudioDownloader::installTools()
{
    if (! lockTools())
    {
        finish ("Cancelled.", 0.0);
        return false;
    }

    const juce::ScopeGuard unlock { [this] { toolsLock.exit(); } };

    removeOldTools();

    for (const auto& tool : tools)
    {
        if (! installTool (tool))
        {
            if (cancelled.load())
                finish ("Cancelled.", 0.0);
            else
                progress.store (0.0);

            return false;
        }
    }

    return true;
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
            if (file.getFileName() != match->file)
                file.deleteFile();
    }
}

// Work folders are deleted after each download. One can stay behind only if the DAW
// closed in the middle of a download, so anything older than a day is left over.
void AudioDownloader::removeOldWorkFolders()
{
    const auto dayAgo = juce::Time::getCurrentTime() - juce::RelativeTime::days (1);

    for (const auto& folder : toolsFolder.getSiblingFile ("work").findChildFiles (juce::File::findDirectories, false))
        if (folder.getLastModificationTime() < dayAgo)
            folder.deleteRecursively();
}

bool AudioDownloader::installTool (const Tool& tool)
{
    if (getToolFile (tool).existsAsFile())
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

    return download.moveFileTo (getToolFile (tool));
}

bool AudioDownloader::downloadFile (const juce::String& url, const juce::File& target, const juce::String& toolName)
{
    setStatus ("Connecting to download " + toolName + "...");
    progress.store (-1.0);

    juce::WebInputStream stream (juce::URL (url), false);
    stream.withConnectionTimeout (20000).withNumRedirectsToFollow (5);

    // While it is registered here, cancel() can stop a connection or read that is waiting.
    {
        const juce::ScopedLock sl (processLock);

        if (cancelled.load())
            return false;

        webStream = &stream;
    }

    const juce::ScopeGuard unregister { [this] { const juce::ScopedLock sl (processLock); webStream = nullptr; } };

    if (! stream.connect (nullptr) || stream.getStatusCode() != 200)
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

    const auto total = stream.getTotalLength();
    juce::HeapBlock<char> buffer (1 << 16);
    juce::int64 done = 0;

    while (! stream.isExhausted())
    {
        if (threadShouldExit())
            return false;

        const auto numRead = stream.read (buffer, 1 << 16);

        if (numRead <= 0)
            break;

        out.write (buffer, (size_t) numRead);
        done += numRead;

        progress.store (total > 0 ? (double) done / (double) total : -1.0);
        setStatus ("First time only: downloading " + toolName + " (" + megabytes (done)
                   + (total > 0 ? " of " + megabytes (total) : juce::String()) + ")");
    }

    out.flush();
    return ! threadShouldExit() && (total <= 0 || done == total);
}

// yt-dlp -U replaces yt-dlp.exe with the newest release. That file is then no longer the
// one pinned above: yt-dlp checks its own update against the checksums it publishes.
// The pinned version comes back only when the version above is changed.
// Returns true if a newer version was installed.
bool AudioDownloader::updateYtDlp()
{
    if (! lockTools())
        return false;

    const juce::ScopeGuard unlock { [this] { toolsLock.exit(); } };

    lastLine = {};
    ytDlpWasUpdated = false;
    step = Step::other;

    const auto exitCode = runTool (getToolFile (tools[ytDlp]), tools[ytDlp].name, { "-U" });
    return exitCode == 0 && ytDlpWasUpdated;
}

// Returns false only if yt-dlp failed (then error says why), so the caller can update
// yt-dlp and try again. Otherwise the status is already set.
bool AudioDownloader::downloadAudio (const juce::File& ffmpeg, juce::String& error)
{
    downloadedPath = {};
    lastError = {};
    lastLine = {};

    // Step 1: yt-dlp downloads the best audio stream, unchanged, into a private work folder.
    // No fixups: the small ffmpeg cannot remux, and step 2 re-encodes the file anyway.
    // For a playlist link, only its first video.
    const auto workFolder = toolsFolder.getSiblingFile ("work").getChildFile (juce::Uuid().toString());
    workFolder.createDirectory();

    const juce::StringArray arguments { "--no-playlist", "--playlist-items", "1",
                                        "--newline", "--progress", "--encoding", "utf-8",
                                        "--ffmpeg-location", ffmpeg.getFullPathName(), "--fixup", "never",
                                        "--no-js-runtimes", "--js-runtimes", "quickjs:" + getToolFile (tools[quickJs]).getFullPathName(),
                                        "-f", "bestaudio/best",
                                        "-o", workFolder.getChildFile ("%(title)s.%(ext)s").getFullPathName(),
                                        "--print", "after_move:SAVED|%(filepath)s",
                                        "--", jobUrl };

    setStatus ("Reading the YouTube page...");
    progress.store (-1.0);
    step = Step::download;

    const auto downloadExitCode = runTool (getToolFile (tools[ytDlp]), tools[ytDlp].name, arguments);

    if (cancelled.load())
    {
        workFolder.deleteRecursively();
        finish ("Cancelled.", 0.0);
        return true;
    }

    if (downloadExitCode != 0 || downloadedPath.isEmpty())
    {
        workFolder.deleteRecursively();
        error = lastError.isNotEmpty() ? lastError : "Download failed: " + lastLine;
        return false;
    }

    // Step 2: ffmpeg converts it into the save folder. -progress reports how far it is.
    const auto& format = formats[(size_t) jobFormat];
    const juce::File downloaded (downloadedPath);
    const auto target = jobFolder.getChildFile (downloaded.getFileNameWithoutExtension() + format.extension);

    juce::StringArray conversion { "-hide_banner", "-nostdin", "-nostats", "-progress", "pipe:1",
                                   "-y", "-i", downloaded.getFullPathName(), "-vn" };
    conversion.addTokens (format.ffmpegArguments, false);
    conversion.add (target.getFullPathName());

    convertingTo = format.kind;
    durationSeconds = 0.0;
    step = Step::convert;
    setStatus ("Converting to " + convertingTo + "...");
    progress.store (-1.0);

    const auto convertExitCode = runTool (ffmpeg, "ffmpeg", conversion);
    step = Step::other;
    workFolder.deleteRecursively();

    if (cancelled.load())
    {
        target.deleteFile();
        finish ("Cancelled.", 0.0);
    }
    else if (convertExitCode == 0 && target.existsAsFile())
    {
        {
            const juce::ScopedLock sl (statusLock);
            savedFile = target;
        }

        finish ("Saved: " + target.getFileName(), 1.0);
    }
    else
    {
        finish ("Converting failed: " + lastLine, 0.0);
    }

    return true;
}

int AudioDownloader::runTool (const juce::File& program, const juce::String& name, const juce::StringArray& arguments)
{
    juce::StringArray command { program.getFullPathName() };
    command.addArray (arguments);

    {
        const juce::ScopedLock sl (processLock);
        process = std::make_unique<juce::ChildProcess>();

        if (cancelled.load() || ! process->start (command))
        {
            process.reset();
            lastLine = "Could not start " + name + ".";
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
        downloadedPath = line.fromFirstOccurrenceOf ("|", false, false);
        return;
    }

    if (step == Step::convert)
    {
        // ffmpeg prints the input's length once, then "-progress" lines like out_time_us=12345678.
        if (line.startsWith ("Duration:"))
            durationSeconds = parseTime (line.fromFirstOccurrenceOf ("Duration:", false, false).upToFirstOccurrenceOf (",", false, false));

        if (line.startsWith ("out_time_us=") && durationSeconds > 0.0)
        {
            const auto seconds = (double) line.fromFirstOccurrenceOf ("=", false, false).getLargeIntValue() / 1.0e6;
            const auto fraction = juce::jlimit (0.0, 1.0, seconds / durationSeconds);
            progress.store (fraction);
            setStatus ("Converting to " + convertingTo + ": " + juce::String (juce::roundToInt (fraction * 100.0)) + "%");
        }

        // Keep progress lines out of lastLine, so an error message is what a failure shows.
        if (line.containsChar ('=') && ! line.containsChar (' '))
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
    else if (line.contains ("Updated yt-dlp to"))
    {
        ytDlpWasUpdated = true;
    }

    if (job == Job::update)
        setStatus (line);
}
