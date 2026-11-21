#!/bin/sh
# Cross-compile the BB10 ARMv7 helpers (libnobind.so, libbbid.so.1, dialhelper).
# Uses qcc directly if already in a BB10 NDK environment, otherwise runs the
# bbndk:accupara-2017 Docker image (override with BBNDK_IMAGE).
set -e
D=$(cd "$(dirname "$0")" && pwd)
BBNDK_IMAGE=${BBNDK_IMAGE:-bbndk:accupara-2017}

compile_cmds='
qcc -Vgcc_ntoarmv7le -Wall -O2 -shared -fPIC -o libnobind.so nobind.c &&
qcc -Vgcc_ntoarmv7le -Wall -O2 -shared -fPIC -Wl,-soname,libbbid.so.1 -o libbbid.so.1 fakebbid.c &&
qcc -Vgcc_ntoarmv7le -Wall -O2 -o dialhelper dialhelper.c -lsocket
'

if command -v qcc >/dev/null 2>&1; then
    (cd "$D" && sh -c "$compile_cmds")
else
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$D":"$D" -w "$D" "$BBNDK_IMAGE" bash -c "$compile_cmds"
fi
ls -l "$D/libnobind.so" "$D/libbbid.so.1" "$D/dialhelper"
