#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="$ROOT/tools/mbedtlsprobe/build"
mkdir -p "$OUT"

docker run --rm --user "$(id -u):$(id -g)" \
    -v "$ROOT:$ROOT" -w "$ROOT/tools/mbedtlsprobe" \
    bbndk:accupara-2017 \
    qcc -Vgcc_ntoarmv7le -O2 \
        -I"$ROOT/third_party/mbedtls/include" \
        main.c "$ROOT/third_party/mbedtls/lib/armv7/libmbedtls_all.a" \
        -lsocket -o build/mbedtlsprobe

echo "Built $OUT/mbedtlsprobe"
