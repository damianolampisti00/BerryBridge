/* dialhelper: WhatsApp calls from the Berry Bridge app in the native Phone app.
 *
 * Runs as root next to our EBMService (/var/rootdata/wacalls-ebm/run.sh). The
 * app is sandboxed and may not write the engine's PPS control object, so it
 * asks here: one TCP line "dial <digits>\n" or "video <digits>\n" on
 * 127.0.0.1:5071, answered with "ok\n" or "err <why>\n". We send the engine
 * initializeCall with the requested media type; it creates the call and opens
 * the BBM Call screen, which sends makeCall itself. Our PPS
 * client stays open a few seconds so the engine hands the call over.
 * The engine adds @domain: the address is the bare number (393331234567).
 * A second instance exits at once (the port is taken).
 *
 * Also a STUN server on 127.0.0.1:3478 (UDP) for the engine's candidate
 * gathering, so it needs no outside one (without any configured, it never
 * starts its audio): a Binding Request gets a success response with the
 * address it came from (MAPPED-ADDRESS, and XOR-MAPPED-ADDRESS when it
 * carries the RFC 5389 magic cookie). Nothing leaves the phone. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 5071
#define CONTROL "/pps/services/rtc/videochat/call/control"
#define STATUS "/pps/services/rtc/videochat/status"
#define HOLD_SECS 10
#define STUN_PORT 3478
#define STUN_MAGIC 0x2112A442UL

static void logf_(const char *fmt, const char *a)
{
    char ts[32];
    time_t t = time(0);
    strftime(ts, sizeof ts, "%m-%d %H:%M:%S", localtime(&t));
    printf("%s ", ts);
    printf(fmt, a);
    printf("\n");
    fflush(stdout);
}

static int registered(void)
{
    char buf[4096];
    int n, fd = open(STATUS, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    return strstr(buf, "isRegistered:b:true") != 0;
}

/* EBM keys media receivers by remote SIP target and can retain a video
 * receiver after a call ends. Give every outgoing call a distinct SIP user;
 * the SIP gateway strips this alphabetic suffix and routes the original
 * phone number to WhatsApp. Keep the suffix letters-only so number
 * normalization cannot accidentally append part of the call id. */
static void call_target(char *out, size_t outlen, const char *num)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
    unsigned long long value = ((unsigned long long)time(0) << 32) |
                               (unsigned int)getpid();
    char tag[15];
    int i;
    for (i = 13; i >= 0; --i) {
        tag[i] = alphabet[value % 26];
        value /= 26;
    }
    tag[14] = 0;
    snprintf(out, outlen, "%s-x%s", num, tag);
}

/* Sends initializeCall; returns the PPS fd (kept open by the caller) or -1. */
static int initialize_call(const char *num, int video, char *why, size_t whylen)
{
    char msg[256], buf[2048], target[48];
    int fd = open(CONTROL, O_RDWR), n;
    time_t end = time(0) + 3;
    if (fd < 0) {
        snprintf(why, whylen, "control: %s", strerror(errno));
        return -1;
    }
    call_target(target, sizeof target, num);
    n = snprintf(msg, sizeof msg, "msg::initializeCall\nid::bb1\ndat:json:{\"address\":\"%s\",\"video\":%s}\n",
                 target, video ? "true" : "false");
    if (write(fd, msg, n) != n) {
        snprintf(why, whylen, "write: %s", strerror(errno));
        close(fd);
        return -1;
    }
    while (time(0) < end) { /* the engine answers res::initializeCall with err:: on failure */
        fd_set f;
        struct timeval tv = {1, 0};
        FD_ZERO(&f);
        FD_SET(fd, &f);
        if (select(fd + 1, &f, 0, 0, &tv) <= 0)
            continue;
        n = read(fd, buf, sizeof buf - 1);
        if (n <= 0)
            break;
        buf[n] = 0;
        if (strstr(buf, "res::initializeCall")) {
            char *e = strstr(buf, "\nerr::");
            if (e) {
                snprintf(why, whylen, "engine: %.100s", e + 6);
                close(fd);
                return -1;
            }
            break;
        }
    }
    return fd;
}

static void serve(int c)
{
    char line[128] = "", why[160] = "", num[24];
    int n = 0, i, video = 0, prefix;
    struct timeval tv = {2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (n < (int)sizeof line - 1) {
        int r = read(c, line + n, 1);
        if (r <= 0 || line[n] == '\n')
            break;
        n++;
    }
    line[n] = 0;
    if (strncmp(line, "dial ", 5) == 0) {
        prefix = 5;
    } else if (strncmp(line, "video ", 6) == 0) {
        prefix = 6;
        video = 1;
    } else {
        const char *usage = "err usage: dial <digits> | video <digits>\n";
        write(c, usage, strlen(usage));
        return;
    }
    for (i = 0; line[prefix + i] && i < (int)sizeof num - 1; i++) {
        char ch = line[prefix + i];
        if (ch < '0' || ch > '9')
            break;
        num[i] = ch;
    }
    num[i] = 0;
    if (i < 6 || i > 15 || line[prefix + i]) {
        write(c, "err bad number\n", 15);
        logf_("refused %s", line);
        return;
    }
    if (!registered()) {
        write(c, "err engine not registered\n", 26);
        logf_("dial %s: engine not registered", num);
        return;
    }
    {
        int fd = initialize_call(num, video, why, sizeof why);
        if (fd < 0) {
            char out[200];
            int m = snprintf(out, sizeof out, "err %s\n", why);
            write(c, out, m);
            logf_("dial failed: %s", why);
            return;
        }
        write(c, "ok\n", 3);
        close(c);
        logf_(video ? "video call %s: ok" : "dial %s: ok", num);
        sleep(HOLD_SECS);
        close(fd);
    }
}

/* One Binding Request in, one success response out. */
static void stun_reply(int u)
{
    unsigned char in[548], out[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(u, in, sizeof in, 0, (struct sockaddr *)&from, &fl), len = 0;
    unsigned long ip = ntohl(from.sin_addr.s_addr), magic;
    unsigned short port = ntohs(from.sin_port);
    if (n < 20 || in[0] != 0x00 || in[1] != 0x01)
        return; /* not a Binding Request */
    magic = ((unsigned long)in[4] << 24) | (in[5] << 16) | (in[6] << 8) | in[7];
    out[0] = 0x01, out[1] = 0x01;       /* Binding Success */
    memcpy(out + 4, in + 4, 16);        /* cookie + transaction id (RFC 3489: 16-byte id) */
    out[20] = 0x00, out[21] = 0x01;     /* MAPPED-ADDRESS */
    out[22] = 0x00, out[23] = 0x08;
    out[24] = 0x00, out[25] = 0x01;     /* IPv4 */
    out[26] = port >> 8, out[27] = port & 0xff;
    out[28] = ip >> 24, out[29] = ip >> 16, out[30] = ip >> 8, out[31] = ip;
    len = 12;
    if (magic == STUN_MAGIC) {
        unsigned short xp = port ^ (STUN_MAGIC >> 16);
        unsigned long xi = ip ^ STUN_MAGIC;
        out[32] = 0x00, out[33] = 0x20; /* XOR-MAPPED-ADDRESS */
        out[34] = 0x00, out[35] = 0x08;
        out[36] = 0x00, out[37] = 0x01;
        out[38] = xp >> 8, out[39] = xp & 0xff;
        out[40] = xi >> 24, out[41] = xi >> 16, out[42] = xi >> 8, out[43] = xi;
        len += 12;
    }
    out[2] = len >> 8, out[3] = len & 0xff;
    sendto(u, out, 20 + len, 0, (struct sockaddr *)&from, fl);
}

int main(void)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    signal(SIGCHLD, SIG_IGN); /* no zombies */
    signal(SIGPIPE, SIG_IGN);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 4) < 0) {
        logf_("already running or port busy: %s", strerror(errno));
        return 0;
    }
    {
        struct sockaddr_in ua;
        int u = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&ua, 0, sizeof ua);
        ua.sin_family = AF_INET;
        ua.sin_port = htons(STUN_PORT);
        ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(u, (struct sockaddr *)&ua, sizeof ua) < 0) {
            logf_("STUN port busy: %s", strerror(errno));
            close(u);
            u = -1;
        }
        logf_("listening on 127.0.0.1:%s (STUN 3478/udp)", "5071");
        for (;;) {
            fd_set f;
            int c, m = s > u ? s : u;
            FD_ZERO(&f);
            FD_SET(s, &f);
            if (u >= 0)
                FD_SET(u, &f);
            if (select(m + 1, &f, 0, 0, 0) <= 0)
                continue;
            if (u >= 0 && FD_ISSET(u, &f))
                stun_reply(u);
            if (!FD_ISSET(s, &f))
                continue;
            c = accept(s, 0, 0);
            if (c < 0)
                continue;
            if (fork() == 0) {
                close(s);
                if (u >= 0)
                    close(u);
                serve(c);
                _exit(0);
            }
            close(c);
        }
    }
}
