#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

// Saves the audio of a YouTube video into a folder, as MP3, FLAC or WAV.
//
// The work is done by three free tools that run as separate programs, so nothing here
// can disturb the audio: yt-dlp (downloads), ffmpeg (converts) and QuickJS (lets yt-dlp
// read YouTube's pages). A small ffmpeg ships inside the plugin. The other two are
// downloaded the first time they are needed, and each download is checked against a
// pinned SHA-256 checksum before it is used.
class AudioDownloader : private juce::Thread
{
public:
    // In the order the menu shows them. Saved by id (getFormatId), so the order can change.
    enum class Format { mp3_192, mp3_128, mp3_256, mp3_320, flac, wav };
    static juce::StringArray getFormatNames();
    static juce::String getFormatId (Format format);
    static Format getFormatFromId (const juce::String& id);   // unknown ids give the default

    struct Tool;

    AudioDownloader();
    ~AudioDownloader() override;

    // Message thread only.
    void startDownload (const juce::String& url, const juce::File& folder, Format format);
    void startUpdate();
    void cancel();

    // Any thread.
    bool isBusy() const                 { return isThreadRunning(); }
    double getProgress() const          { return progress.load(); }   // 0 to 1, or -1 while unknown
    juce::String getStatus() const;
    juce::File getSavedFile() const;    // the file the last download saved, if any

private:
    enum class Job { download, update };
    enum class Step { other, download, convert };

    void run() override;
    bool installTools();
    bool lockTools();
    void removeOldTools();
    void removeOldWorkFolders();
    bool installTool (const Tool& tool);
    bool downloadFile (const juce::String& url, const juce::File& target, const juce::String& toolName);
    bool updateYtDlp();
    bool downloadAudio (const juce::File& ffmpeg, juce::String& error);
    juce::File getToolFile (const Tool& tool) const;
    int runTool (const juce::File& program, const juce::String& name, const juce::StringArray& arguments);
    void handleOutputLine (const juce::String& line);
    void setStatus (const juce::String& newStatus);
    void finish (const juce::String& newStatus, double newProgress);

    const juce::File toolsFolder;

    // Shared by every Stream Bridge on this PC, so two of them never change the tools
    // folder at the same time.
    juce::InterProcessLock toolsLock { "StreamBridgeTools" };

    Job job = Job::download;
    juce::String jobUrl;
    juce::File jobFolder;
    Format jobFormat = Format::mp3_192;

    // cancel() uses these to stop a running tool or download right away.
    juce::CriticalSection processLock;
    std::unique_ptr<juce::ChildProcess> process;
    juce::WebInputStream* webStream = nullptr;

    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancelled { false };

    mutable juce::CriticalSection statusLock;
    juce::String status;
    juce::File savedFile;

    // Used only by the download thread.
    Step step = Step::other;
    juce::String downloadedPath, lastError, lastLine, convertingTo;
    double durationSeconds = 0.0;
    bool ytDlpWasUpdated = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioDownloader)
};
