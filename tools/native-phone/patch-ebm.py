#!/usr/bin/env python3
"""Patch a stock BlackBerry 10 BBM EBMService binary into EBMService.wa.

The stock EBMService binary is proprietary BlackBerry software and is not
distributed here. Pull it from your own rooted BB10 device:

    /apps/sys.bbm.*/native/EBMService

Usage:
    ./patch-ebm.py <input-EBMService> <output-EBMService.wa>
"""
import hashlib
import os
import sys

ORIG_SHA256 = "cb2dfeae81fd86fb905edaacca19d27c5490f6229c91396191202e2b5ab522c4"
PATCHED_SHA256 = "48a05b7fd0d9731761f5dc57ec1081b915f772f074c407f351b9f78fbdef3610"

# (offset, expected_orig_hex, patched_hex, description)
PATCHES = [
    (0x072D5E, "40f3fd80", "00bf00bf", "nop; nop: avoid 24s PPS client-still-connected reject on outgoing calls"),
    (0x0FE900, "57e0",     "0de1",     "skip SRTP key wait when BBMCore is absent (outgoing calls)"),
    (0x0FE9B1, "80",       "b8",       "complete skip-SRTP branch target"),
    (0x106526, "01f01a82", "00bf00bf", "nop; nop: skip early OpenSession on non-ICE 200 OK (fixes outgoing video)"),
    (0x13D998, "41f28831", "40f22c11", "shorten ICE timer 3 from 5000 ms to 300 ms"),
    (0x13DD48, "abb1",     "00bf",     "nop: do not fail call when ICE timer expires"),
    (0x23ECFA, "62",       "77",       "redirect /pps/services/bbid/status -> /pps/services/wbid/status"),
]


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <stock-EBMService> <output-EBMService.wa>", file=sys.stderr)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = bytearray(f.read())

    digest = hashlib.sha256(data).hexdigest()
    if digest != ORIG_SHA256:
        print(
            f"Error: unexpected input SHA-256:\n"
            f"  got:      {digest}\n"
            f"  expected: {ORIG_SHA256}\n"
            f"Refusing to patch an unrecognized EBMService build.",
            file=sys.stderr,
        )
        return 1

    for offset, orig_hex, new_hex, desc in PATCHES:
        orig = bytes.fromhex(orig_hex)
        new = bytes.fromhex(new_hex)
        assert len(orig) == len(new)
        actual = bytes(data[offset : offset + len(orig)])
        if actual != orig:
            print(
                f"Error at 0x{offset:x} ({desc}): expected {orig_hex}, found {actual.hex()}",
                file=sys.stderr,
            )
            return 1
        data[offset : offset + len(new)] = new

    out_digest = hashlib.sha256(data).hexdigest()
    if out_digest != PATCHED_SHA256:
        print(
            f"Error: unexpected output SHA-256 {out_digest} (expected {PATCHED_SHA256})",
            file=sys.stderr,
        )
        return 1

    with open(dst, "wb") as f:
        f.write(data)
    os.chmod(dst, 0o755)
    print(f"Patched {dst} ({len(data)} bytes, sha256 {out_digest[:12]}...)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
