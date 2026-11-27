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
- **Only the copy is delayed**, by the "Stream buffer" (default 20 ms since 2026-11-17,
  was 40 ms; 0–250 ms). Saved projects keep their own value.
- **Cost must be minuscule.** The owner accepts a change only if its CPU/RAM/disk cost is
  tiny, and wants that cost stated up front. Nothing may add locks, allocations or system
  calls on the host audio thread beyond what is already there.

## Repository layout

| Path | What it is |
|---|---|
| `Source/PluginProcessor.*` | The plugin. Parameters, state, device selection, status/warning text, reconnect logic (`handleAsyncUpdate`). |
| `Source/BridgeOutput.*` | The bridge: lock-free FIFO from the host thread to a WASAPI output thread, resampling with clock-drift correction, stream health measurement, device open/close/reconnect events. |
| `Source/PluginEditor.*` | The window: controls, stream health (last 10 seconds, always shown), warnings, downloader UI, drag-out label, tooltips, dynamic height (`layOut`). |
| `Source/AudioDownloader.*` | YouTube downloader thread: installs pinned tools, runs yt-dlp then ffmpeg, progress parsing, auto-update-and-retry. |
| `Source/WindowsAudioCheck.*` | Registry checks: "Listen to this device" on the cable's recording side, and the "looks like a virtual device" name heuristic. |
| `tests/LatencyTest.cpp` | Console app: drives the processor like Ableton (48 kHz, 128-sample blocks) and checks output == input, 0 latency, CPU, RAM, dropouts. |
| `tests/DownloadTest.cpp` | Console app: runs the downloader without a DAW. `DownloadTest.exe <link> <folder> [format index]`. |
| `tests/CopyQualityTest.cpp` | Console app: the viewers' copy against a perfect copy with test tones (unconverted + skips vs the Lagrange converter, at 20/50/100 ppm). |
| `tests/FfmpegIdenticalTest.ps1` | PowerShell: the small ffmpeg against a full ffmpeg of the same version, all 6 formats, byte by byte. |
| `ffmpeg/build.sh` | Builds the small (~2.4 MB) static ffmpeg + LAME in MSYS2 UCRT64. |
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
- **Buffer setting changed**: jump to the new fill at once (may click once).
- **Health resets only when the user changes the Stream buffer setting** (`healthSettingMs`
  in the callback). Restarts, reconnects and a different minimum from another device do not
  reset it; the owner wants "since the Stream buffer was last changed".
- **Pile-up** (the output side paused while the host kept sending): when the FIFO holds
  more than target + one host block + `pileUpMarginMs` (2 ms) for `pileUpCallbacks` (3)
  callbacks in a row, the extra is thrown away at once (one short skip for viewers), so the
  copy is back at the Stream buffer's delay right away. Tested 2026-11-17 at 17 ms: no jumps
  in 20 seconds of normal playing, a 29 ms pile-up gone after 33 ms, a 2.7 ms one left to
  the normal drift correction. `getPileUpJumpCount()` counts jumps for tests (not shown).
- **Health**: the lowest margin (spare samples after a read) and dropouts are always measured.
  A dropout is counted only if the host pushed within the last 250 ms, so stopping the
  engine, exporting or bypassing the plugin is not reported as a dropout.
  Measured cost (2026-11-17, `HealthBench`): 15–19 ns per block on the host thread (one
  `getMillisecondCounter` + one atomic store) and 9–11 ns per device callback.
- **Health display** (editor, once per second, only while the window is open): keeps the
  last 10 seconds, one entry per second (lowest margin, dropouts). A big coloured line says
  stable (≥ 3 ms spare, green), close to gaps (1–3 ms, orange), almost a gap (< 1 ms, red)
  or gaps (any dropout, red); under it "What to do:" gives the buffer that keeps 3 ms
  spare. It suggests lowering only after a full 10 seconds and only if there was no
  dropout since the Stream buffer was last changed. The "Stream buffer" label turns
  orange/red when it should be raised. Changing the Stream buffer starts everything over;
  a stop or restart only starts the 10-second window over. Below it, "Since the Stream
  buffer was last changed: …" shows the dropout count. The owner chose to keep "0 seconds
  ago" (not "just now"). There is no Details button any more; everything is always shown.
- **No slow draining** (the owner's decision on 2026-11-17, reversing an earlier one): the
  old behaviour let a pile-up of up to 3× the target drain at ~2 ms per second (24 s at
  17 ms, 58 s at 40 ms), keeping viewers that much further behind. The owner prefers one
  short skip and the setting's delay right away.

Device modes and delay readout:

- `deviceType` is WASAPI **shared low-latency** (IAudioClient3); devices open with the
  smallest buffer the driver allows. If that fails, `tryOpen` falls back to a lazily made
  normal shared-mode type. `isLowLatencyMode()` is true only when the buffer really is
  smaller than Windows' default.
- The window shows "Copy delay now": the averaged FIFO fill (`getBufferedMs`) plus
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
- **Sample rate differs** (lowest priority of the three): the output runs at another rate
  than the host. In WASAPI low-latency mode Windows does no conversion and a device offers
  only its Windows format, so the reopen-at-host-rate check cannot fix it; the copy then
  goes through the Lagrange converter, which loses treble (measured 2026-11-17: −0.1 dB at
  10 kHz, −0.6 at 14 k, −1.1 at 16 k, −3.1 at 20 k on average, with added tones 25–38 dB
  below 10–14 kHz content). The warning gives the mmsys.cpl steps. It is also rechecked
  from `handleAsyncUpdate` when the host rate changes.
- **Same rate on both sides: no converting.** `copyWithSlip` passes the copy through
  unchanged; when the smoothed FIFO fill is more than `slipThresholdSamples` (32) off the
  target, one sample is skipped or repeated, the read position sliding by one sample over
  `slipFadeSamples` (64) with 5-point Lagrange math (same as JUCE's). Slips happen
  drift × 48000 times per second (about 1 at 20 ppm, 5 at 100 ppm). Measured 2026-11-17
  against a perfect copy, at 50 ppm: level 0 dB at every frequency (Lagrange today:
  −1.1 dB at 16 kHz, −2.5 at 20 k); difference −57 dB at 10 kHz (Lagrange −35), −39 at
  16 k (−16), −32 at 20 k (−8); also better in the worst 10 ms. Different rates still use
  the Lagrange converter. JUCE's WindowedSinc was rejected: flat, but ~4.5% of a CPU core.

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
  `--enable-small`, asm on, static, w32threads. Result ≈ 2.4 MB, only Windows system DLLs.
- **Keep ffmpeg's assembly code on** (needs `nasm`). Without it the Opus and AAC decoders
  round a few samples differently, and the MP3 encoder turns that into different files.
  With it, every format is byte-identical to a full ffmpeg 9.0.2 build (checked
  2026-11-17 with both the Opus and the AAC version of a video). The owner wants identical
  output, so check that again after any change to the build options.
- Writes `ffmpeg-LICENSE.txt` (LGPL 2.1) and `ffmpeg-README.txt` (exact sources) next to
  it; both ship inside the plugin for LGPL compliance.
- To add a codec or bump versions: edit the script, update the SHA-256, rebuild. CI caches
  the result keyed on the script's hash, so any edit triggers a rebuild (~4 min).
- MSYS2 pitfalls: `tar` treats `C:/...` as a remote host, so the script converts paths
  with `cygpath`; the script must stay LF (`.gitattributes` has `*.sh text eol=lf`).

## Building

- Needs Visual Studio 2022 (or Build Tools) with **Desktop development with C++**, plus
  MSYS2 in `C:\msys64` with `mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-nasm make diffutils`
  for ffmpeg.
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
- The repo is **public** since 2026-11-27, and so are its releases. Anything the plugin
  downloads at runtime must come from public URLs.

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
- `CopyQualityTest.exe`: must end with "… beats the converter everywhere: YES" and exit 0.
  Run it after any change to `BridgeOutput`'s copy or drift correction.
- `powershell -ExecutionPolicy Bypass -File tests\FfmpegIdenticalTest.ps1 [-Link <YouTube link>]`
  after `build.bat`: must end with "…byte-identical…: YES" and exit 0. Run it after any
  change to `ffmpeg/build.sh`. It downloads a pinned full ffmpeg once into `build\`, makes
  its own Opus and AAC test sounds (pink noise), and with `-Link` also tests a real video's
  Opus and AAC versions. Verified 2026-11-17: YES with the current build (24 of 24
  identical); NO with the old `--disable-x86asm` build, so it does catch that.
- A real check in Ableton: hear yourself once, Discord viewers hear the DAW, the status line
  says "Playing to CABLE Input …" and the health line says "Stream stable".

## Working with the owner

- **Propose changes one at a time as yes/no (multiple-choice) questions**, with the cost
  of each stated. Don't hand over a long list to pick from by number.
- State the cost (CPU/RAM/disk/complexity) of every proposed change; tiny costs only.
- Use plain words in the UI and in explanations: "seconds", not "s". Explain audio and
  Windows concepts simply; the owner is a musician first.
- Work on a branch, commit, and push so CI builds it; the owner checks the Actions tab and
  tests the artifact. Merge to `main` only when the owner says it works. Ask before pushing
  to a new branch or to `main`.
- Commit messages: one short title line only. No body, no `Co-Authored-By` line, no
  "Generated with Claude Code". The same goes for pull request descriptions.
- Finish with one clear "what to do now" step, in numbered clicks. Long option lists or
  several parallel next steps confuse the owner ("I didn't understand what to do now").
- The owner can't share CI logs easily: ask them to click the red step and paste the lines
  with `error` (there is no log-download button on the run summary page).

## Decisions already made (don't re-propose)

Rejected: fading the copy in and out, a send level meter, a "Discord can hear this"
input check, warning about two plugin instances, protecting existing files from overwrite,
slow draining after a pile-up (was the choice until 2026-11-17; now the extra is dropped at
once), suggesting at least 1.5× the buffer after a dropout, "just now" instead of
"0 seconds ago", a "Video for Ableton (MP4)" download format (so Ableton's video window
could show the YouTube picture in sync).

The browser-sync issue, as explained to the owner on 2026-11-17: Discord takes the
browser's sound early (inside Windows), while the owner hears it late through the
Scarlett's Windows (non-ASIO) driver (about 100 ms, measured by ear) and plays along with
that, so the guitar reaches the stream late. The browser holds its picture back to match
the late sound, so on stream the YouTube picture is also behind its own sound. The fix is
playing the song inside Ableton (downloader + drag to track); Discord cannot delay a
screen share's picture or another app's sound.

Accepted and implemented on branch `small-ffmpeg`: bundled small ffmpeg, auto-reconnect,
reopen at the host rate, "Listen"/real-speaker warnings, tooltips, 250 ms buffer cap,
playlist → first video, inter-instance tool lock, work-folder cleanup, prompt cancel,
drag-to-Ableton, Show in folder, conversion progress, auto-update-and-retry, FLAC, MP3 192
default, installer, Get VB-Cable button, README, version in the window, dropping
`juce_audio_utils`, dead-code removal, the "Copy delay now" readout, WASAPI low-latency
mode with fallback, ffmpeg assembly code on (identical output), no Details button: the
stream health for the last 10 seconds is always shown, updated once per second, with a
clear "What to do:" line, health reset only on a Stream buffer change, sample-rate warning,
"put it last on Master" text, unconverted copy with skips when the rates match, default
Stream buffer 20 ms, the two quality tests, MP3 320 stays, pile-up dropped at once.

## Where we left off (2026-11-17)

The owner wrapped the plugin up: branch `small-ffmpeg` was merged into `main` (fast-forward)
and released as **v0.3.0** (tag on the commit "Drop piled-up audio at once instead of
draining it slowly"). CI builds the release zip and installer from the tag.

| Commit | What | CI |
|---|---|---|
| `e772708` | Small ffmpeg bundled in the plugin | built |
| `8331034` | "seconds" instead of "s" | built |
| `7c75957` | Auto-reconnect, reopen at host rate | built |
| `0fa99f4` | The big batch (warnings, health line, downloader, installer, version…) | failed: CMake needed VERSION on the test apps |
| `9f86460` | CLAUDE.md + README | same failure |
| `c19923e` | VERSION on the test apps | **green**, installer artifact produced |
| `e2c5355` | WASAPI low-latency mode + "Delay of the copy now" readout | built |
| `8e6e50e` | No Co-Authored-By line in CLAUDE.md | built |
| `c01b5f5` | ffmpeg assembly code back on (identical output) | **green** |
| `5199ae1` | Health for the last 10 seconds, sample-rate warning, unconverted copy, 20 ms default, quality tests | not checked |
| v0.3.0 | Pile-up dropped at once instead of draining slowly | release build |

Not yet tried by the owner in a real Ableton + Discord session (only with the test
programs): the health display, sample-rate warning, unconverted copy, 20 ms default and
pile-up jump. If something is wrong there, fix it on a branch and release v0.3.1.

If the owner comes back:

1. If the v0.3.0 release build was red, get the error lines and fix.
2. The owner may report "Copy delay now: … (plugin … + Windows …, low-latency / normal
   mode)" from a real session. Whether VB-Cable supports low-latency mode is unknown.

Findings from 2026-11-17:

- "Listen to this device" on CABLE Output is now **off** on the owner's PC (it was on and
  caused the doubled, ~800 ms late guitar).
- The owner uses **Discord screen share only** (entire screen), no Discord microphone.
  Backing tracks play in a **browser**; viewers hear the guitar ~200 ms late (test "B":
  still late with "Listen" off). Ableton's latency and the plugin's buffer setting were
  both small. Recommended fix given: play the track inside Ableton.
- Asked "isn't there another driver?": no. Discord only hears audio that passes through
  Windows' shared audio engine; exclusive mode / kernel streaming bypass it, ASIO bridges
  (VB-Audio, Voicemeeter) add buffering, other virtual cables don't matter because Discord
  captures the process audio before the cable, and Focusrite Loopback is a microphone
  (separate, unsynced path in Discord).
- Decided 2026-11-17: "MP3 320 kbps (larger file)" stays in the format menu, as do all
  other formats. (The owner had asked why 320 exists if YouTube is only ~128–160 kbps:
  it is the most faithful MP3 copy, practically the same as 192.)
- The **other PC** (not the home PC; used on 2026-11-16 daytime and 2026-11-17) has Visual
  Studio Build Tools 2026 **with** C++, so it builds the plugin with `build.bat`, and MSYS2
  in `C:\msys64` with gcc and nasm. Its network breaks MSYS2's `curl` (TLS certificate
  check fails), so download the two source tarballs with PowerShell and put them in
  `build\ffmpeg\work\` first; `build.sh` checks their SHA-256 and skips the download.
  Its project path contains a space (`C:\Users\Kursat Uzunlu\...`); deep scratch folders
  hit MAX_PATH with JUCE, so build helper tools in a short folder like `C:\msys64\tmp`.
- The **home PC** (the one used on the evening of 2026-11-16) has Visual Studio 2022 Professional **without**
  the C++ workload, so it cannot build the plugin; MSYS2 is installed (UCRT64 gcc), which
  builds ffmpeg and allows a syntax-only check of the sources:
  `g++ -std=c++17 -fsyntax-only -w -U__MINGW32__ -U__MINGW64__ -I libs/JUCE/modules
  -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 <file>`
  and look only at errors in `Source/` or `tests/` (JUCE itself rejects MinGW; this only
  catches mistakes in our code). It caught a real one once (`juce::File = {}` ambiguous).

## Licensing

JUCE is used under the free **JUCE Starter** licence (JUCE 9 EULA, chosen 2026-11-27):
allowed while yearly revenue or funding is up to $20,000, counting donations, ads and
sponsorship. Above that, a paid JUCE licence is needed. The repo has no LICENSE file on
purpose ("all rights reserved"): the EULA (section 2.3) forbids putting JUCE under an
open-source licence that forces sharing the source or free redistribution, so don't add
AGPL, GPL or MIT. Never copy JUCE's source into the repo; it stays a submodule of the
official JUCE repo. ffmpeg/LAME are LGPL 2.1 (notices ship in the bundle). VB-Cable is not
redistributed, only linked.
