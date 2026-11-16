#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

// Saves the audio of a YouTube video into a folder, as MP3 or WAV.
//
// The work is done by three free tools that run as separate programs, so nothing here
// can disturb the audio: yt-dlp (downloads), ffmpeg (converts) and Deno (lets yt-dlp
// read YouTube's pages). They are downloaded the first time they are needed, and each
// download is checked against a pinned SHA-256 checksum before it is used.
class AudioDownloader : private juce::Thread
{
public:
    enum class Format { mp3_320, mp3_256, mp3_192, mp3_128, wav };
    static juce::StringArray getFormatNames();

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

private:
    enum class Job { download, update };

    void run() override;
    bool installTool (const Tool& tool);
    bool downloadFile (const juce::String& url, const juce::File& target, const juce::String& toolName);
    juce::File getToolFile (const Tool& tool, const char* fileName) const;
    int runYtDlp (const juce::StringArray& arguments);
    void handleOutputLine (const juce::String& line);
    void setStatus (const juce::String& newStatus);

    const juce::File toolsFolder;

    Job job = Job::download;
    juce::String jobUrl;
    juce::File jobFolder;
    Format jobFormat = Format::mp3_320;

    juce::CriticalSection processLock;
    std::unique_ptr<juce::ChildProcess> process;

    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancelled { false };

    mutable juce::CriticalSection statusLock;
    juce::String status;

    // Used only by the download thread.
    juce::String savedFile, lastError, lastLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioDownloader)
};
