#ifndef TLSPOLICY_HPP_
#define TLSPOLICY_HPP_

#include <QHostAddress>
#include <QString>

// Certificates are checked (the original accepted any, VerifyNone plus
// ignoreSslErrors: on a hostile Wi-Fi anyone could pose as the server, read
// the messages and take the Beeper token). Only a server on the local network
// may present a certificate that doesn't verify: a Beeper Desktop reached on
// the LAN often has a self-signed one.
// Shared by the app (BerryBridgeUI/src includes it from here) and the service.
inline bool tlsLocalHost(const QString &host)
{
    if (host == "localhost" || host.endsWith(".local", Qt::CaseInsensitive)) return true;
    QHostAddress a;
    if (!a.setAddress(host) || a.protocol() != QAbstractSocket::IPv4Protocol) return false;
    const quint32 ip = a.toIPv4Address();
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == (0xAC100000u >> 20) // 172.16/12
            || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;                           // 192.168/16, 169.254/16
}

#endif /* TLSPOLICY_HPP_ */
