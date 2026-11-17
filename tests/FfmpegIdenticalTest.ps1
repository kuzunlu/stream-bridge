# Checks that the small ffmpeg (ffmpeg/build.sh) makes byte-identical files to a full ffmpeg
# build of the same version, for every format the YouTube downloader offers.
#
# Usage, from the project folder, after build.bat:
#   powershell -ExecutionPolicy Bypass -File tests\FfmpegIdenticalTest.ps1
#   powershell -ExecutionPolicy Bypass -File tests\FfmpegIdenticalTest.ps1 -Link <YouTube link>
#
# The test sound is made here (pink noise, encoded as Opus and as AAC, the two formats YouTube
# serves), so no download from YouTube is needed. -Link also tests the real Opus and AAC
# versions of a video; that needs yt-dlp and QuickJS, which the plugin or DownloadTest.exe
# installs on first use. Exit code 0 means every file was identical.

param (
    [string] $SmallFfmpeg = "$PSScriptRoot\..\build\ffmpeg\ffmpeg.exe",
    [string] $Link = ""
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# The full build to compare with. Its version must be the one build.sh uses.
$FullVersion = "9.0.2"
$FullUrl = "https://github.com/GyanD/codexffmpeg/releases/download/$FullVersion/ffmpeg-$FullVersion-essentials_build.zip"
$FullSha256 = "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"

# The same settings as the formats in Source/AudioDownloader.cpp.
$Formats = [ordered] @{
    "mp3_192" = @(".mp3",  "-c:a libmp3lame -b:a 192k")
    "mp3_128" = @(".mp3",  "-c:a libmp3lame -b:a 128k")
    "mp3_256" = @(".mp3",  "-c:a libmp3lame -b:a 256k")
    "mp3_320" = @(".mp3",  "-c:a libmp3lame -b:a 320k")
    "flac"    = @(".flac", "-c:a flac -sample_fmt s16")
    "wav"     = @(".wav",  "-c:a pcm_s16le")
}

$root = (Resolve-Path "$PSScriptRoot\..").Path
$work = Join-Path $root "build\ffmpeg-identical-test"
$fullExe = Join-Path $root "build\ffmpeg-full-$FullVersion\ffmpeg.exe"

$buildScript = Get-Content (Join-Path $root "ffmpeg\build.sh") -Raw
if ($buildScript -notmatch "FFMPEG_VERSION=$([regex]::Escape($FullVersion))\s") {
    throw "ffmpeg/build.sh no longer uses ffmpeg $FullVersion. Update FullVersion, FullUrl and FullSha256 in this script."
}

if (-not (Test-Path $SmallFfmpeg)) { throw "Small ffmpeg not found at $SmallFfmpeg. Run build.bat first." }

# Download the full ffmpeg once and keep only ffmpeg.exe.
if (-not (Test-Path $fullExe)) {
    $zip = Join-Path $root "build\ffmpeg-full-$FullVersion.zip"
    Write-Host "Downloading the full ffmpeg $FullVersion (about 115 MB, first time only)..."
    Invoke-WebRequest -Uri $FullUrl -OutFile $zip -UseBasicParsing

    if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $FullSha256) {
        Remove-Item $zip
        throw "The full ffmpeg download did not match its checksum."
    }

    New-Item -ItemType Directory -Force (Split-Path $fullExe) | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)

    try {
        $entry = $archive.Entries | Where-Object { $_.FullName -like "*/bin/ffmpeg.exe" } | Select-Object -First 1
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $fullExe, $true)
    }
    finally {
        $archive.Dispose()
    }

    Remove-Item $zip
}

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null

# Test sounds: 20 seconds of pink noise, as Opus (48000 Hz) and AAC (44100 Hz).
$sources = [ordered] @{}
$noise = "anoisesrc=d=20:c=pink:r=48000:a=0.3:seed=1,aformat=channel_layouts=stereo"
& $fullExe -hide_banner -nostdin -loglevel error -y -f lavfi -i $noise -c:a libopus -b:a 128k "$work\noise-opus.webm"
& $fullExe -hide_banner -nostdin -loglevel error -y -f lavfi -i $noise -ar 44100 -c:a aac -b:a 128k "$work\noise-aac.m4a"
$sources["pink noise, Opus"] = "$work\noise-opus.webm"
$sources["pink noise, AAC"] = "$work\noise-aac.m4a"

# Optionally the real Opus and AAC versions of a YouTube video.
if ($Link -ne "") {
    $tools = Join-Path $env:LOCALAPPDATA "Stream Bridge\tools"
    $ytDlp = Get-ChildItem "$tools\yt-dlp-*\yt-dlp.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    $qjs = Get-ChildItem "$tools\quickjs-ng-*\qjs.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $ytDlp -or -not $qjs) { throw "yt-dlp or QuickJS not found in $tools. Run DownloadTest.exe once to install them." }

    foreach ($kind in @(@("webm", "YouTube, Opus"), @("m4a", "YouTube, AAC"))) {
        & $ytDlp.FullName --no-playlist --fixup never --quiet --no-warnings --no-js-runtimes --js-runtimes "quickjs:$($qjs.FullName)" `
            -f "bestaudio[ext=$($kind[0])]" -o "$work\youtube.%(ext)s" -- $Link
        $file = "$work\youtube.$($kind[0])"
        if (Test-Path $file) { $sources[$kind[1]] = $file } else { Write-Host "No $($kind[1]) version found for this video." }
    }
}

$allIdentical = $true
Write-Host ""
Write-Host ("{0,-20} {1,-9} {2}" -f "source", "format", "result")

foreach ($sourceName in $sources.Keys) {
    foreach ($id in $Formats.Keys) {
        $ext = $Formats[$id][0]
        $formatArgs = $Formats[$id][1].Split(" ")
        $tag = ($sourceName -replace "[^a-zA-Z]", "")
        $fullOut = "$work\$tag-$id-full$ext"
        $smallOut = "$work\$tag-$id-small$ext"

        & $fullExe -hide_banner -nostdin -nostats -loglevel error -y -i $sources[$sourceName] -vn @formatArgs $fullOut
        & $SmallFfmpeg -hide_banner -nostdin -nostats -loglevel error -y -i $sources[$sourceName] -vn @formatArgs $smallOut

        $same = (Test-Path $fullOut) -and (Test-Path $smallOut) -and ((Get-FileHash $fullOut).Hash -eq (Get-FileHash $smallOut).Hash)
        if (-not $same) { $allIdentical = $false }
        Write-Host ("{0,-20} {1,-9} {2}" -f $sourceName, $id, $(if ($same) { "identical" } else { "DIFFERENT" }))
    }
}

Write-Host ""
if ($allIdentical) {
    Write-Host "Small ffmpeg makes byte-identical files to the full ffmpeg ${FullVersion}: YES"
    exit 0
}

Write-Host "Small ffmpeg makes byte-identical files to the full ffmpeg ${FullVersion}: NO"
exit 1
