# Native Phone app calls (EBMService setup)

On rooted BlackBerry 10 devices, Berry Bridge can route WhatsApp voice and H.264 video calls through the native Phone app by running a patched copy of BB10's BBM Video engine (`EBMService`) against Berry Bridge's local SIP/RTP relay (`127.0.0.1:5060`).

The stock `EBMService` binary is proprietary BlackBerry software and is **not** included in this repository. Instead, `patch-ebm.py` patches the copy already present on your device into a separate file (`/var/rootdata/wacalls-ebm/EBMService.wa`), leaving BBM's original binary untouched.

## Contents

- `nobind.c` -> `libnobind.so`: `LD_PRELOAD` shim that disables `SO_BINDTODEVICE` and rewrites UDP binds to `INADDR_ANY` so the engine can exchange RTP with Berry Bridge's loopback relay.
- `fakebbid.c` -> `libbbid.so.1`: minimal BlackBerry ID stub so `EBMService` registers to SIP without BlackBerry ID servers.
- `dialhelper.c` -> `dialhelper`: root helper listening on `127.0.0.1:5071` (triggers outgoing voice/video calls via PPS for the sandboxed Berry Bridge app) and `127.0.0.1:3478` (local STUN responder so ICE candidate gathering completes offline).
- `patch-ebm.py`: verifies the stock `EBMService` SHA-256 and applies the 7 binary patches (fake BBID PPS path, skip SRTP key wait without BBMCore, 300 ms ICE timer 3, non-fatal ICE timer expiry, outgoing video `OpenSession` fix, 24 s PPS reconnect fix).
- `ebm.conf`, `run.sh`, `swap.sh`, `restore.sh`: runtime configuration and start/stop scripts under `/var/rootdata/wacalls-ebm/`.
- `addon-start.sh`: AutoRoot V4 boot addon (`/var/addons/wacallsebm/init/start.sh`) that runs `swap.sh` after boot once BBM's stock `EBMService` has started.
- `build.sh`: cross-compiles `libnobind.so`, `libbbid.so.1`, and `dialhelper` using the BB10 NDK (`qcc` or the `bbndk:accupara-2017` Docker image).
- `install.sh`: pulls stock `EBMService` from the phone over SSH, patches it, pushes all files to `/var/rootdata/wacalls-ebm/` and `/var/addons/wacallsebm/`, and starts it.

## Install

Make sure SSH as root works to your BB10 device (e.g. `ssh q10`), then run:

```sh
./install.sh q10
```

If you downloaded the prebuilt `native-phone-helpers-armv7.tar.gz` from GitHub Releases, unpack it into this directory first so `install.sh` skips `build.sh` (no BB10 NDK or Docker needed).

Pass `--no-boot-addon` to skip installing `/var/addons/wacallsebm`.

## Undo / Uninstall

- Temporarily stop our copy (reverts on reboot):
  ```sh
  ssh q10 /var/rootdata/wacalls-ebm/restore.sh
  ```
- Remove permanently:
  ```sh
  ssh q10 'rm -rf /var/rootdata/wacalls-ebm /var/addons/wacallsebm'
  ```
