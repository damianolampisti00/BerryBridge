#ifndef SIPRELAY_HPP_
#define SIPRELAY_HPP_

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

// Relay for the phone's native call engine (EBMService, the BBM Video
// engine run as WaCalls' SIP terminal, see WaCalls sipgateway.go): the
// engine talks SIP to 127.0.0.1:5060 and RTP to 127.0.0.1:40000, and this
// carries both through the gateway link (CallLink), so the native Phone app
// gets WhatsApp calls wherever the link works, not only on the home Wi-Fi.
// Wire format: WaCalls bbsiptunnel.go (binary kinds 0x10 SIP, 0x11 RTP,
// {"type":"sip.open"} / {"type":"sip.close"}).
// Event driven only (no timers): idle, it costs no wakeups.
// Lives in CallLink's network thread.
class SipRelay : public QObject
{
    Q_OBJECT
public:
    static const quint16 kSipPort = 5060;
    static const quint16 kRtpPort = 40000;

    explicit SipRelay(QObject *parent = 0);

    bool engineConnected() const;
    bool rtpActiveRecently(qint64 windowMs = 5000) const;
    // From the gateway.
    void fromGatewaySip(const QByteArray &bytes);
    void fromGatewayRtp(quint16 enginePort, const QByteArray &datagram);
    void gatewayClosedSip(); // {"type":"sip.close"} from the gateway
    // The link opened / closed.
    void linkUp();
    void linkDown();

signals:
    // To the gateway (CallLink frames them).
    void sipOut(const QByteArray &bytes);
    void rtpOut(quint16 enginePort, const QByteArray &datagram);
    void controlOut(const QByteArray &json);

private slots:
    void onNewConnection();
    void onEngineData();
    void onEngineGone();
    void onRtp();

private:
    QTcpServer *m_server;
    QTcpSocket *m_engine;
    QUdpSocket *m_udp;
    bool m_linkUp;
    bool m_announced; // sip.open sent on this link connection
    QElapsedTimer m_lastRtp;
};

#endif /* SIPRELAY_HPP_ */
