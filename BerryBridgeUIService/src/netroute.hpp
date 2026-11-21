#ifndef NETROUTE_HPP_
#define NETROUTE_HPP_

#include <QAbstractSocket>
#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Returns the local IPv4 source address the QNX kernel routing table currently
// picks to reach `peer`, using an unconnected-peer UDP connect()+getsockname()
// (no network packets sent). Returns a null QHostAddress on failure.
static inline QHostAddress preferredLocalAddress(const QHostAddress &peer)
{
    if (peer.isNull() || peer.protocol() != QAbstractSocket::IPv4Protocol) return QHostAddress();
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return QHostAddress();

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(443);
    dst.sin_addr.s_addr = htonl(peer.toIPv4Address());

    QHostAddress out;
    if (::connect(fd, reinterpret_cast<const struct sockaddr *>(&dst), sizeof(dst)) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        memset(&local, 0, sizeof(local));
        if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &len) == 0 &&
            local.sin_family == AF_INET) {
            out = QHostAddress(ntohl(local.sin_addr.s_addr));
        }
    }
    ::close(fd);
    if (out == QHostAddress::Any) return QHostAddress();
    return out;
}

// True if `addr` is still assigned to a local UP+RUNNING interface. Used
// during an active call to distinguish "better route appeared, old route still
// alive (defer)" from "old interface vanished mid-call (migrate now)".
static inline bool isLocalAddressAssigned(const QHostAddress &addr)
{
    if (addr.isNull()) return false;
    foreach (const QNetworkInterface &iface, QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & (QNetworkInterface::IsUp | QNetworkInterface::IsRunning)) !=
            (QNetworkInterface::IsUp | QNetworkInterface::IsRunning)) {
            continue;
        }
        foreach (const QNetworkAddressEntry &entry, iface.addressEntries()) {
            if (entry.ip() == addr) return true;
        }
    }
    return false;
}

#endif /* NETROUTE_HPP_ */
