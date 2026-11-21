#!/bin/sh
# Deploy the native Phone app call helpers (EBMService.wa, dialhelper,
# libnobind.so, libbbid.so.1, scripts and AutoRoot boot addon) to a rooted BB10
# device over SSH.
#
# Usage: ./install.sh <ssh-target> [--no-boot-addon]
# Example: ./install.sh q10
set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <ssh-target> [--no-boot-addon]" >&2
    exit 2
fi

TARGET=$1
BOOT_ADDON=1
if [ "$2" = "--no-boot-addon" ]; then
    BOOT_ADDON=0
fi

D=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. Build helpers if not already present next to this script
if [ ! -f "$D/libnobind.so" ] || [ ! -f "$D/libbbid.so.1" ] || [ ! -f "$D/dialhelper" ]; then
    echo "Building BB10 helpers..."
    sh "$D/build.sh"
fi

# 2. Pull the stock EBMService binary from the phone and patch it locally
echo "Locating stock EBMService on $TARGET..."
REMOTE_EBM=$(ssh "$TARGET" 'ls /apps/sys.bbm.*/native/EBMService 2>/dev/null | head -n 1')
if [ -z "$REMOTE_EBM" ]; then
    echo "Error: /apps/sys.bbm.*/native/EBMService not found on $TARGET" >&2
    exit 1
fi
echo "Pulling $REMOTE_EBM..."
scp "$TARGET:$REMOTE_EBM" "$TMP/EBMService.orig"
python3 "$D/patch-ebm.py" "$TMP/EBMService.orig" "$TMP/EBMService.wa"

# 3. Push files to /var/rootdata/wacalls-ebm
echo "Installing to $TARGET:/var/rootdata/wacalls-ebm..."
ssh "$TARGET" 'mkdir -p /var/rootdata/wacalls-ebm/lib'
scp "$TMP/EBMService.wa" "$D/dialhelper" "$D/ebm.conf" "$D/run.sh" "$D/swap.sh" "$D/restore.sh" \
    "$TARGET:/var/rootdata/wacalls-ebm/"
scp "$D/libnobind.so" "$D/libbbid.so.1" "$TARGET:/var/rootdata/wacalls-ebm/lib/"
ssh "$TARGET" 'chmod 755 /var/rootdata/wacalls-ebm/EBMService.wa /var/rootdata/wacalls-ebm/dialhelper /var/rootdata/wacalls-ebm/run.sh /var/rootdata/wacalls-ebm/swap.sh /var/rootdata/wacalls-ebm/restore.sh /var/rootdata/wacalls-ebm/lib/libnobind.so /var/rootdata/wacalls-ebm/lib/libbbid.so.1'

# 4. Install AutoRoot boot addon
if [ "$BOOT_ADDON" = "1" ]; then
    echo "Installing AutoRoot boot addon (/var/addons/wacallsebm)..."
    ssh "$TARGET" 'mkdir -p /var/addons/wacallsebm/init /var/addons/wacallsebm/data'
    scp "$D/addon-start.sh" "$TARGET:/var/addons/wacallsebm/init/start.sh"
    ssh "$TARGET" 'chmod 755 /var/addons/wacallsebm/init/start.sh'
fi

# 5. Swap running EBMService
echo "Starting patched EBMService on $TARGET..."
ssh "$TARGET" '/bin/sh /var/rootdata/wacalls-ebm/swap.sh'
