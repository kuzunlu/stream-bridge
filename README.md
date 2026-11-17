# Stream Bridge

A VST3 plugin for Windows. Put it on your DAW's Master channel. It sends a copy of the
audio to a Windows output device (for example a virtual cable), so Discord screen share
can hear a DAW that uses an ASIO interface.

- Your own sound is not delayed or changed. The plugin reports 0 latency.
- Only the copy is delayed, by the Stream buffer: 20 ms to start with. The window tells you
  if your PC needs more, and by how much.
- It also has a small YouTube-to-audio downloader (MP3, FLAC or WAV).

## Download

Get the newest release from the **Releases** page (on the right side of the repository page):

- `Stream-Bridge-Setup-….exe`: the installer. Easiest.
- `Stream-Bridge-v….zip`: the plugin alone, to copy by hand.

You need:

- 64-bit Windows
- A virtual audio cable, for example the free [VB-Audio Virtual Cable](https://vb-audio.com/Cable/).
  The installer offers to open its download page if you have none.

## Install

1. Run the installer. Or unzip the zip and copy the `Stream Bridge.vst3` folder to
   `C:\Program Files\Common Files\VST3\`.
2. Rescan plugins in your DAW. In Ableton: Preferences → Plug-ins → Rescan.
3. Put **Stream Bridge** on the Master channel and pick your virtual cable as Output
   (for VB-Cable: **CABLE Input**). It picks a cable by itself if it finds one.
   Put it **last** on the Master channel, after any limiter or EQ, so viewers hear those too.
   Set the cable to the same sample rate as your DAW (the plugin warns you if they differ).
4. In Discord, share your screen. Viewers hear the DAW.

The installer has an uninstaller (Windows Settings → Apps). It also removes the tools the
YouTube downloader fetched.

## Using it

Hover over any control to see what it does.

- **Send audio** turns the copy on or off. Off: viewers hear nothing. Your own sound never changes.
- **Volume** changes the copy only.
- **Stream buffer** is how far behind the copy plays (0–250 ms). A bigger buffer is safer,
  a smaller one has less delay for viewers.
- The line under it tells you how the stream is doing:
  - **Stream OK, no dropouts.** Nothing to do.
  - **Dropout**: viewers heard a short gap. It tells you which buffer size would have
    avoided it. Set the slider to that.
  - **Details** shows the measurements behind it: the safety margin (how much spare audio
    there was at the tightest moment) and the smallest buffer that can work.
- If the output drops out (Windows sleep, a driver reset), the plugin reconnects by itself.

### If you hear yourself twice

You should never hear the copy yourself. If you do, the plugin shows a warning saying why.
The usual causes:

- Windows' **"Listen to this device"** is on for **CABLE Output**. Turn it off:
  Win+R → `mmsys.cpl` → Recording → CABLE Output → Properties → Listen → untick
  "Listen to this device".
- The plugin's Output is your real speakers or headphones instead of the cable.

### YouTube to audio

Paste a link, choose a folder and a format, and press **Download**. When it is saved, drag
the file from the plugin straight onto an Ableton track, or press **Show in folder**.

- **MP3 192 kbps** is the default. YouTube's audio is only about 128–160 kbps, so higher MP3
  rates make bigger files without better sound. **FLAC** and **WAV** are exact copies.
- For a playlist link, only its first video is saved.
- The first download fetches two free tools (about 25 MB): [yt-dlp](https://github.com/yt-dlp/yt-dlp)
  and [QuickJS](https://github.com/quickjs-ng/quickjs). They are checked against fixed checksums
  and kept in `%LOCALAPPDATA%\Stream Bridge`.
- If a download fails, the plugin gets the newest yt-dlp and tries once more.
  **Update yt-dlp** does the update by hand.
- Some antivirus programs wrongly flag `yt-dlp.exe`, because it is a packed Python program.
  It is the official release from the yt-dlp project.

## Making a new release

Push a version tag. GitHub builds the plugin and the installer and publishes the Release
by itself:

```
git tag v0.3.0
git push origin v0.3.0
```

The plugin shows its version (from the tag) next to "YouTube to audio".

Every normal push also builds everything. Those builds are under the **Actions** tab, under
*Artifacts*: **Stream-Bridge-VST3** (the plugin) and **Stream-Bridge-Installer**.

## Build it yourself

Needs Visual Studio or the free Build Tools for Visual Studio, with
"Desktop development with C++".

The YouTube downloader uses a small ffmpeg (about 2 MB) that ships inside the plugin.
`build.bat` builds it the first time, which needs [MSYS2](https://www.msys2.org) in `C:\msys64`
with its compiler. Install that once from an MSYS2 terminal:

```
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make diffutils
```

Then:

```
git clone --recursive https://github.com/kuzunlu/stream-bridge.git
cd stream-bridge
build.bat
```

The plugin ends up in `build\StreamBridge_artefacts\Release\VST3\`.

`build\LatencyTest_artefacts\Release\LatencyTest.exe` checks that the plugin adds no
delay and measures its CPU and memory use. `build\DownloadTest_artefacts\Release\DownloadTest.exe`
runs the YouTube downloader without a DAW.

## Licences

Stream Bridge is built with [JUCE](https://juce.com), which is licensed under AGPLv3 or a
commercial JUCE licence. The bundled ffmpeg is FFmpeg with LAME, under the LGPL 2.1
(see `ffmpeg-LICENSE.txt` and `ffmpeg-README.txt` inside the plugin).
