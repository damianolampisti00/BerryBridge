/* libnobind.so: LD_PRELOAD shim for our EBMService (the BBM Video engine run
 * as WaCalls' SIP terminal, /var/rootdata/wacalls-ebm/run.sh).
 *
 * The engine binds each UDP socket to one interface (SO_BINDTODEVICE bcm0)
 * and to that interface's address. Such a socket cannot exchange datagrams
 * with the Berry Bridge relay on 127.0.0.1:40000 (the path for calls away
 * from home): io-pkt only hands it packets that arrive on the Wi-Fi itself.
 * So: SO_BINDTODEVICE is a no-op, and a UDP bind to an interface address
 * binds the same port on INADDR_ANY instead. Routing then picks the
 * interface, loopback included.
 * If *:port is taken, the bind FAILS with EADDRINUSE (2026-11-03): the
 * engine walks its port range binding each candidate, and the old fallback
 * (keep the interface address, which succeeds next to a *:port socket) put
 * the video channel on the audio's own port numbers: our video then reached
 * the audio socket ("audio_jb ... PT 109, expected 0", video received=0). */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
static int (*real_bind)(int, const struct sockaddr *, socklen_t);

int setsockopt(int s, int level, int name, const void *val, socklen_t len)
{
    if (!real_setsockopt)
        real_setsockopt = (int (*)(int, int, int, const void *, socklen_t))dlsym(RTLD_NEXT, "setsockopt");
    if (level == SOL_SOCKET && name == SO_BINDTODEVICE) {
        fprintf(stderr, "[nobind] fd %d: SO_BINDTODEVICE %.16s ignored\n", s, val ? (const char *)val : "");
        return 0;
    }
    return real_setsockopt(s, level, name, val, len);
}

int bind(int s, const struct sockaddr *addr, socklen_t len)
{
    if (!real_bind)
        real_bind = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "bind");
    if (addr && addr->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        unsigned long ip = ntohl(in->sin_addr.s_addr);
        int type = 0;
        socklen_t tl = sizeof type;
        if (ip != INADDR_ANY && (ip >> 24) != 127 &&
            getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_DGRAM) {
            struct sockaddr_in any;
            memcpy(&any, in, sizeof any);
            any.sin_addr.s_addr = htonl(INADDR_ANY);
            if (real_bind(s, (const struct sockaddr *)&any, sizeof any) == 0) {
                fprintf(stderr, "[nobind] fd %d: udp bind %s:%d -> *:%d\n", s, inet_ntoa(in->sin_addr),
                        ntohs(in->sin_port), ntohs(in->sin_port));
                return 0;
            }
            if (errno == EADDRINUSE && in->sin_port != 0) {
                fprintf(stderr, "[nobind] fd %d: udp *:%d busy (%s): EADDRINUSE\n", s, ntohs(in->sin_port),
                        inet_ntoa(in->sin_addr));
                return -1;
            }
            fprintf(stderr, "[nobind] fd %d: udp *:%d failed (%s), keeping %s\n", s, ntohs(in->sin_port),
                    strerror(errno), inet_ntoa(in->sin_addr));
        }
    }
    return real_bind(s, addr, len);
}
