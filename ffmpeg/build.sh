#!/usr/bin/env bash
# Builds a small ffmpeg.exe that can only do what the YouTube downloader needs: read the
# audio that YouTube serves (Opus, AAC and a few others) and write MP3, FLAC or WAV.
# A normal ffmpeg build is over 100 MB. This one is a few MB.
#
# Runs in MSYS2 with the UCRT64 environment. build.bat calls it.
# Usage: build.sh <output folder>
# The output folder gets ffmpeg.exe and its licence files. Sources are cached in <output folder>/work.

set -euo pipefail

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
FFMPEG_URL=https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz

LAME_VERSION=3.100
LAME_SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
LAME_URL=https://deb.debian.org/debian/pool/main/l/lame/lame_$LAME_VERSION.orig.tar.gz

out=$(realpath -m "$(cygpath -u "${1:?usage: build.sh <output folder>}")")   # tar misreads C:/ paths
work=$out/work
prefix=$work/prefix
jobs=$(nproc)

if ! command -v gcc >/dev/null || ! command -v make >/dev/null; then
    echo "MSYS2 needs a compiler. In an MSYS2 terminal run: pacman -S --needed mingw-w64-ucrt-x86_64-gcc make" >&2
    exit 1
fi

mkdir -p "$work"

# Downloads a file once and checks it against its pinned SHA-256.
fetch()
{
    local url=$1 sha=$2 file=$work/$(basename "$1")

    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        curl -sSfL --retry 3 -o "$file.part" "$url"
        echo "$sha  $file.part" | sha256sum -c --quiet -
        mv "$file.part" "$file"
    fi

    echo "$file"
}

lameArchive=$(fetch "$LAME_URL" "$LAME_SHA256")
ffmpegArchive=$(fetch "$FFMPEG_URL" "$FFMPEG_SHA256")

# LAME, the MP3 encoder, as a static library.
rm -rf "$work/lame-$LAME_VERSION" "$prefix"
tar -xzf "$lameArchive" -C "$work"
(
    cd "$work/lame-$LAME_VERSION"
    ./configure --build=x86_64-w64-mingw32 --host=x86_64-w64-mingw32 --prefix="$prefix" \
                --disable-shared --enable-static --disable-frontend --disable-decoder --disable-gtktest \
                CFLAGS="-O2"
    make -j"$jobs"
    make install
)

# ffmpeg with everything turned off except the parts listed here.
rm -rf "$work/ffmpeg-$FFMPEG_VERSION"
tar -xJf "$ffmpegArchive" -C "$work"
(
    cd "$work/ffmpeg-$FFMPEG_VERSION"
    ./configure --prefix="$prefix" \
                --disable-everything --disable-autodetect --disable-network \
                --disable-doc --disable-debug --disable-ffplay --disable-ffprobe \
                --disable-avdevice --disable-swscale \
                --disable-pthreads --enable-w32threads \
                --disable-x86asm --enable-small \
                --enable-libmp3lame \
                --enable-protocol=file,pipe \
                --enable-demuxer=matroska,mov,mp3,ogg,flac,wav,aac,ac3,eac3,mpegts \
                --enable-parser=opus,aac,aac_latm,mpegaudio,vorbis,flac,ac3 \
                --enable-decoder=opus,aac,aac_latm,mp3float,vorbis,flac,alac,ac3,eac3,pcm_s16le,pcm_s24le,pcm_f32le \
                --enable-encoder=libmp3lame,flac,pcm_s16le \
                --enable-muxer=mp3,flac,wav \
                --enable-filter=aresample,aformat,anull,atrim \
                --pkg-config-flags=--static \
                --extra-cflags="-I$prefix/include" \
                --extra-ldflags="-L$prefix/lib -static"
    make -j"$jobs"
    make install
)

cp "$prefix/bin/ffmpeg.exe" "$out/ffmpeg.exe"
strip "$out/ffmpeg.exe"

# LGPL: ship the licence and say where the exact sources come from.
cp "$work/ffmpeg-$FFMPEG_VERSION/COPYING.LGPLv2.1" "$out/ffmpeg-LICENSE.txt"
cat > "$out/ffmpeg-README.txt" <<EOF
ffmpeg.exe is FFmpeg $FFMPEG_VERSION (https://ffmpeg.org) with LAME $LAME_VERSION (https://lame.sourceforge.io),
both under the GNU Lesser General Public License 2.1 (see ffmpeg-LICENSE.txt).
It was built without changes from these sources by ffmpeg/build.sh in the Stream Bridge repository:
  $FFMPEG_URL  (SHA-256 $FFMPEG_SHA256)
  $LAME_URL  (SHA-256 $LAME_SHA256)
EOF

echo "Built $out/ffmpeg.exe ($(du -h "$out/ffmpeg.exe" | cut -f1))"
