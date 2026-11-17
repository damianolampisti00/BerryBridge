# mbed TLS 2.28.9

This directory contains the ARMv7 headers and static library used by
BerryBridge's BB10 HTTPS transport. The library was built for QNX/BB10 from
Mbed TLS 2.28.9 and is licensed under Apache-2.0; see [LICENSE](LICENSE).

The diagnostic client in `tools/mbedtlsprobe` verifies the server certificate
chain and hostname and reports the negotiated TLS version and cipher. Its
test CA bundle is copied into the application assets as `cacert.pem`.
