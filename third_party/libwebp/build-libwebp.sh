#!/bin/bash
# Rebuilds lib/armv7/libwebpdecoder.a + libwebpdemux.a (libwebp 1.4.0, decoder and
# demuxer only) with the BB10 NDK's qcc, in the bbndk Docker image bb10-build uses.
# Berry Bridge needs them for stickers: WhatsApp sends them as WebP, and BB10's image
# codecs (img_codec_*.so) don't read WebP. No NEON: stickers are small.
#   ./build-libwebp.sh      (downloads the release tarball and checks its sha256)
set -euo pipefail
VER=1.4.0
SHA=61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

curl -sSLf -o "$WORK/libwebp.tar.gz" \
    "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$VER.tar.gz"
echo "$SHA  $WORK/libwebp.tar.gz" | sha256sum -c -
tar xzf "$WORK/libwebp.tar.gz" -C "$WORK"

docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$WORK":"$WORK" \
    -w "$WORK/libwebp-$VER" bbndk:accupara-2017 \
    make -f makefile.unix -j"$(nproc)" src/libwebpdecoder.a src/demux/libwebpdemux.a \
        CC="qcc -Vgcc_ntoarmv7le" AR=ntoarmv7-ar \
        CFLAGS="-O2 -DNDEBUG -fPIC -fvisibility=hidden -std=gnu99"

S="$WORK/libwebp-$VER"
mkdir -p "$HERE/lib/armv7" "$HERE/include/webp"
cp "$S/src/libwebpdecoder.a" "$S/src/demux/libwebpdemux.a" "$HERE/lib/armv7/"
cp "$S/src/webp/decode.h" "$S/src/webp/demux.h" "$S/src/webp/mux_types.h" "$S/src/webp/types.h" \
   "$HERE/include/webp/"
cp "$S/COPYING" "$S/PATENTS" "$HERE/"
echo "libwebp $VER -> $HERE/lib/armv7"
