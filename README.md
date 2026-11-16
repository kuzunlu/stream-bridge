# Stream Bridge

A VST3 plugin for Windows. Put it on your DAW's Master channel. It sends a copy of the
audio to a Windows output device (for example a virtual cable), so Discord screen share
can hear a DAW that uses an ASIO interface.

- Your own sound is not delayed or changed. The plugin reports 0 latency.
- Only the copy is delayed, by about 40 ms.

## Download

Get the newest `.zip` from the **Releases** page (on the right side of the repository page).

You need:

- 64-bit Windows
- A virtual audio cable, for example the free [VB-Audio Virtual Cable](https://vb-audio.com/Cable/)

## Install

1. Unzip it and copy the `Stream Bridge.vst3` folder to `C:\Program Files\Common Files\VST3\`.
2. Rescan plugins in your DAW. In Ableton: Preferences → Plug-ins → Rescan.
3. Put **Stream Bridge** on the Master channel and pick your virtual cable as Output.
4. In Discord, share your screen. Viewers hear the DAW.
