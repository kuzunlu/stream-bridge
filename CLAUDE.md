# CLAUDE.md

Guide for Claude sessions working on this repository. The owner works on it from more
than one PC, so everything a new session needs to know lives here, not in local memory.

## What this is

**Stream Bridge** is a Windows-only VST3 plugin (JUCE, C++17). It goes on the DAW's Master
channel (the owner uses Ableton Live with a Focusrite interface over ASIO) and sends a copy
of the audio to a Windows output device through WASAPI shared mode, usually the VB-Audio
virtual cable ("CABLE Input"). Discord screen share can then hear a DAW that otherwise
bypasses the Windows audio engine through ASIO.

It also has a **YouTube-to-audio downloader** (yt-dlp + a bundled small ffmpeg + QuickJS)
that saves MP3/FLAC/WAV and lets the user drag the file onto an Ableton track.

Core promises, which must never break:

- **The host's audio is never changed or delayed.** `processBlock` passes the buffer through
  untouched and the plugin reports 0 latency. `tests/LatencyTest.cpp` checks this bit-exactly.
- **Only the copy is delayed**, by the "Stream buffer" (default 40 ms, 0–250 ms).
- **Cost must be minuscule.** The owner accepts a change only if its CPU/RAM/disk cost is
  tiny, and wants that cost stated up front. Nothing may add locks, allocations or system
  calls on the host audio thread beyond what is already there.

## Repository layout

| Path | What it is |
|---|---|
| `Source/PluginProcessor.*` | The plugin. Parameters, state, device selection, status/warning text, reconnect logic (`handleAsyncUpdate`). |
| `Source/BridgeOutput.*` | The bridge: lock-free FIFO from the host thread to a WASAPI output thread, resampling with clock-drift correction, stream health measurement, device open/close/reconnect events. |
| `Source/PluginEditor.*` | The window: controls, health line + Details, warnings, downloader UI, drag-out label, tooltips, dynamic height (`layOut`). |
| `Source/AudioDownloader.*` | YouTube downloader thread: installs pinned tools, runs yt-dlp then ffmpeg, progress parsing, auto-update-and-retry. |
| `Source/WindowsAudioCheck.*` | Registry checks: "Listen to this device" on the cable's recording side, and the "looks like a virtual device" name heuristic. |
| `tests/LatencyTest.cpp` | Console app: drives the processor like Ableton (48 kHz, 128-sample blocks) and checks output == input, 0 latency, CPU, RAM, dropouts. |
| `tests/DownloadTest.cpp` | Console app: runs the downloader without a DAW. `DownloadTest.exe <link> <folder> [format index]`. |
| `ffmpeg/build.sh` | Builds the small (~2.2 MB) static ffmpeg + LAME in MSYS2 UCRT64. |
| `installer/StreamBridge.iss` | Inno Setup 6 installer script. |
| `build.bat` | Local/CI build: builds ffmpeg once via MSYS2, then CMake + Ninja with Visual Studio's toolchain. |
| `.github/workflows/build.yml` | CI on every push; a `v*` tag also publishes a GitHub Release. |
| `libs/JUCE` | JUCE as a git submodule (shallow). Run `git submodule update --init` after cloning. |

## How the bridge works (BridgeOutput)

Threads:

- **Host audio thread**: `processBlock` → `bridge.push()`. Copies the block into a lock-free
  `juce::AbstractFifo` (2 ch × 2^17 samples ≈ 1 MB) and stores `lastPushTime`. Skipped while
  the host renders offline (`isNonRealtime()`), so exports don't flood the FIFO.
- **WASAPI device thread** (JUCE's): `audioDeviceIOCallbackWithContext` pulls from the FIFO,
  resamples with `LagrangeInterpolator`, applies the smoothed gain, writes to the device.
- **Message thread**: opening/closing the device, editor, status text, reconnect handling.

Key behaviour in the callback:

- **Target fill** = the user's buffer setting, clamped between the minimum workable value
  (one device block + one host block + 32 samples of jitter) and `fifoSize / 2`.
- **Priming**: after a start or a dropout, output silence until the FIFO holds the target.
- **Drift correction**: the two devices have different clocks, so the resampling ratio is
  nudged by at most ±0.2% (`maxSpeedCorrection`) towards the target fill (smoothed).
- **Buffer setting changed**: jump to the new fill at once (may click once) and reset health.
- **Overfull** (`> min(4 × target, fifoSize − 4096)`): skip ahead.
- **Health**: the lowest margin (spare samples after a read) and dropouts are always measured.
  A dropout is counted only if the host pushed within the last 250 ms, so stopping the
  engine, exporting or bypassing the plugin is not reported as a dropout.
- **Over-full drains slowly on purpose**: after a hiccup the FIFO may hold up to 4× the
  target and drift correction drains it at 0.2% speed. The owner chose to keep this rather
  than jump back (which would click).

Device modes and delay readout:

- `deviceType` is WASAPI **shared low-latency** (IAudioClient3); devices open with the
  smallest buffer the driver allows. If that fails, `tryOpen` falls back to a lazily made
  normal shared-mode type. `isLowLatencyMode()` is true only when the buffer really is
  smaller than Windows' default.
- Details shows "Delay of the copy now": the averaged FIFO fill (`getBufferedMs`) plus
  JUCE's output latency for the device (`getWindowsOutputMs`). The cable and Discord add
  their own delay after that.
- Known timing issue: if the backing track plays in a **browser** while the guitar goes
  through Ableton, Discord gets the browser audio directly and the guitar via the bridge,
  so viewers hear the guitar late (~200 ms measured by the owner). Part of that is outside
  the plugin (the owner plays along with browser audio they hear late through Windows).
  The real fix is playing the track inside Ableton (the downloader + drag-to-track exists
  for this).

Reconnect (costs nothing in steady state):

- JUCE's WASAPI code calls `audioDeviceStopped()` on the message thread when Windows ends
  the session (sleep, driver reset, device removed). If we did not close it on purpose
  (`closingOnPurpose`) and it had run for over 1 s, `onReconnectNeeded` fires → the
  processor's `triggerAsyncUpdate()` → `handleAsyncUpdate()` reopens the device.
- `BridgeOutput` also listens to `AudioIODeviceType::Listener::audioDeviceListChanged()`
  (JUCE already receives these Windows notifications) and asks to reconnect only while not
  running. Opening a device never changes the device list, so there is no loop.
- Sample-rate changes inside Windows are handled by JUCE itself (it reopens).
- `prepareToPlay` triggers the same async check when the host rate differs from the device
  rate; the device is reopened only if it supports the host rate (`couldUseHostSampleRate`).
- Note: picking the already-selected item in a JUCE `ComboBox` does not fire `onChange`, so
  the Refresh button doubles as a manual reconnect.

Saved state (XML from the APVTS tree): parameters `enabled`, `gain`, `bufferMs`, plus
properties `outputDevice`, `downloadFolder`, `downloadFormatId` (a string id, so the menu
order can change). Old properties `downloadFormat` (index) and `bufferTuning` are ignored.

## Warnings (WindowsAudioCheck)

Checked when an output is opened, when the editor opens, and on Refresh. Never polled.

- **"Listen to this device"**: for each active recording endpoint under
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{id}\Properties`
  whose sound-card name (`{b3f8fa53-0004-438e-9003-51a46e139bfc},6`) matches the text in
  the last parentheses of our output's name, read `{24dbb0fc-9311-4b3d-9cf0-18ff155639d4},1`.
  It is a PROPVARIANT: byte 0 = VT_BOOL (11), bytes 8–9 = 0xFFFF when on. This caused the
  owner's "I hear my guitar twice, ~800 ms late" problem.
- **Real speaker output**: `looksVirtual()` is a name heuristic (CABLE, Virtual, VB-Audio,
  Voicemeeter, Wave Link, Steam Streaming). If no output looks virtual, the editor shows a
  "Get VB-Cable" button.

## YouTube downloader (AudioDownloader)

- Runs on its own `juce::Thread`; tools run as child processes, so nothing touches audio.
- **Tools** live in `%LOCALAPPDATA%\Stream Bridge\tools\<name-version>\`. yt-dlp and QuickJS
  (`qjs.exe`; yt-dlp needs a JS runtime for YouTube) are downloaded on first use and checked
  against pinned SHA-256s in `tools[]`. Changing a version there makes the plugin fetch the
  new one; `removeOldTools()` deletes folders/files not in the list (this also cleaned up
  the old 105 MB ffmpeg download from earlier versions).
- **ffmpeg is not downloaded**: it ships in `Stream Bridge.vst3/Contents/Resources/ffmpeg.exe`
  (`findFfmpeg()` also looks next to the .exe for the test programs).
- A named `juce::InterProcessLock` ("StreamBridgeTools") guards installing, cleaning and
  updating tools, so several plugin instances can't break each other.
- Work folders (`%LOCALAPPDATA%\Stream Bridge\work\<uuid>`) are deleted after each job;
  ones older than a day are leftovers from a crash and are removed at the next download.
- yt-dlp arguments: `--no-playlist --playlist-items 1` (a playlist link saves only its first
  video), `--fixup never` (the small ffmpeg can't remux, and we re-encode anyway),
  `--js-runtimes quickjs:<path>`, `-f bestaudio/best`, `--print after_move:SAVED|%(filepath)s`.
- ffmpeg converts with `-progress pipe:1 -nostats`; the "Duration:" line plus `out_time_us=`
  give a real progress bar. Progress lines are kept out of `lastLine` so failures show errors.
- **Formats** (menu order = `AudioDownloader::Format`): MP3 192 (default; YouTube's audio is
  ~128–160 kbps), MP3 128, MP3 256, MP3 320, FLAC (s16), WAV (s16). Existing files with
  the same name are overwritten (`-y`); the owner chose to keep it that way.
- **Auto-update and retry**: if yt-dlp fails, run `yt-dlp -U` once; if it printed
  "Updated yt-dlp to", try the download again. "Update yt-dlp" does the same by hand. The
  updated exe is no longer the pinned one (yt-dlp verifies its own update); the owner
  accepted this.
- Cancelling kills the child process and cancels a `juce::WebInputStream` mid-read, so
  closing the DAW during a download doesn't hang.

## The small ffmpeg (ffmpeg/build.sh)

- FFmpeg 9.0.2 + LAME 3.100 from pinned, SHA-256-checked tarballs (FFmpeg's was also
  verified against its GPG release key `FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8`).
- `--disable-everything --disable-autodetect`, then only: demuxers matroska, mov, mp3, ogg,
  flac, wav, aac, ac3, eac3, mpegts; decoders opus, aac, mp3, vorbis, flac, alac, ac3, eac3,
  pcm; encoders libmp3lame, flac, pcm_s16le; muxers mp3, flac, wav; a few audio filters.
  `--enable-small`, no asm, static, w32threads. Result ≈ 2.2 MB, only Windows system DLLs.
- Writes `ffmpeg-LICENSE.txt` (LGPL 2.1) and `ffmpeg-README.txt` (exact sources) next to
  it; both ship inside the plugin for LGPL compliance.
- To add a codec or bump versions: edit the script, update the SHA-256, rebuild. CI caches
  the result keyed on the script's hash, so any edit triggers a rebuild (~4 min).
- MSYS2 pitfalls: `tar` treats `C:/...` as a remote host, so the script converts paths
  with `cygpath`; the script must stay LF (`.gitattributes` has `*.sh text eol=lf`).

## Building

- Needs Visual Studio 2022 (or Build Tools) with **Desktop development with C++**, plus
  MSYS2 in `C:\msys64` with `mingw-w64-ucrt-x86_64-gcc make diffutils` for ffmpeg.
- `build.bat` → `build\StreamBridge_artefacts\Release\VST3\Stream Bridge.vst3`. It builds
  `build\ffmpeg\ffmpeg.exe` once if missing, then always reconfigures CMake (so the version
  from `git describe` stays current).
- If a PC lacks the C++ workload, don't install it without asking (several GB). Push the
  branch and let CI compile instead.
- The VST3 links `juce_audio_devices` + `juce_cryptography` (not `juce_audio_utils`, which
  was unused). Runtime is static (`/MT`), so no VC++ redistributable is needed.
- Source files use CRLF in the working tree (`* text=auto`, `*.bat` CRLF, `*.sh` LF). When
  editing with scripts, keep the existing line endings.

## Versioning, CI and releases

- Version = `git describe --tags --match "v[0-9]*"`, e.g. `v0.3.0` or `v0.3.0-2-gabc1234`.
  CMake passes the numeric part to `juce_add_plugin VERSION` and the full text as
  `STREAM_BRIDGE_VERSION`, shown in the window. CI checks out with `fetch-depth: 0` for tags.
- CI (`windows-latest`): restore cached ffmpeg or install the MSYS2 compiler → `build.bat`
  → check ffmpeg is in the bundle → upload **Stream-Bridge-VST3** → build the installer with
  Inno Setup (installed via choco if missing) → upload **Stream-Bridge-Installer**.
  Actions are pinned by commit SHA with a version comment.
- Release: `git tag vX.Y.Z && git push origin vX.Y.Z` → CI publishes the zip + installer.
  Never push a tag unless the owner asks.
- The repo is **private**; release files can't be downloaded without access. Anything the
  plugin downloads at runtime must come from public URLs.

## Installer (installer/StreamBridge.iss)

Admin install into `{commoncf64}\VST3\Stream Bridge.vst3`. If no render endpoint's sound
card name contains "VB-Audio" or "Virtual", the finish page offers to open the VB-Cable
download page. The uninstaller also removes `{localappdata}\Stream Bridge`. Keep `AppId`
unchanged forever. Unsigned, so Windows SmartScreen will warn.

## Testing

There are no unit tests. Verify with:

- `LatencyTest.exe [device] [bufferMs]`: must print "Output exactly equal to input … YES"
  and exit 0. Needs a virtual cable.
- `DownloadTest.exe <link> <folder> [0-5]`: must end with "Saved: …".
- A real check in Ableton: hear yourself once, Discord viewers hear the DAW, the status line
  says "Playing to CABLE Input …" and "Stream OK".
- The ffmpeg build can be tested alone: convert Opus/AAC/MP3 inputs to MP3/FLAC/WAV.

## Working with the owner

- **Propose changes one at a time as yes/no (multiple-choice) questions**, with the cost
  of each stated. Don't hand over a long list to pick from by number.
- State the cost (CPU/RAM/disk/complexity) of every proposed change; tiny costs only.
- Use plain words in the UI and in explanations: "seconds", not "s". Explain audio and
  Windows concepts simply; the owner is a musician first.
- Work on a branch, commit, and push so CI builds it; the owner checks the Actions tab and
  tests the artifact. Merge to `main` only when the owner says it works. Ask before pushing
  to a new branch or to `main`.
- End commit messages with the `Co-Authored-By` line given in the session's instructions.

## Decisions already made (don't re-propose)

Rejected: fading the copy in and out, a send level meter, a "Discord can hear this"
input check, warning about two plugin instances, protecting existing files from overwrite,
jumping back quickly when the FIFO is over-full.

Accepted and implemented on branch `small-ffmpeg`: bundled small ffmpeg, auto-reconnect,
reopen at the host rate, "Listen"/real-speaker warnings, always-on health line with
Details, tooltips, 250 ms buffer cap, playlist → first video, inter-instance tool lock,
work-folder cleanup, prompt cancel, drag-to-Ableton, Show in folder, conversion progress,
auto-update-and-retry, FLAC, MP3 192 default, installer, Get VB-Cable button, README,
version in the window, dropping `juce_audio_utils`, dead-code removal, the "Delay of the
copy now" readout, WASAPI low-latency mode with fallback.

## Licensing

JUCE is AGPLv3 or commercial. If the plugin is shared without its source, a commercial
JUCE licence may be needed; the owner has been told. ffmpeg/LAME are LGPL 2.1 (notices ship
in the bundle). VB-Cable is not redistributed, only linked.
