#include "siprelay.hpp"

#include <QDebug>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

SipRelay::SipRelay(QObject *parent) :
        QObject(parent), m_server(new QTcpServer(this)), m_engine(0), m_udp(new QUdpSocket(this)),
        m_linkUp(false), m_announced(false)
{
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
    if (!m_server->listen(QHostAddress::LocalHost, kSipPort))
        qWarning() << "[SIPRELAY] cannot listen on SIP port:" << m_server->errorString();
    if (!m_udp->bind(QHostAddress::LocalHost, kRtpPort))
        qWarning() << "[SIPRELAY] cannot bind RTP port:" << m_udp->errorString();
    connect(m_udp, SIGNAL(readyRead()), this, SLOT(onRtp()));
}

bool SipRelay::engineConnected() const
{
    return m_engine && m_engine->state() == QAbstractSocket::ConnectedState;
}

void SipRelay::onNewConnection()
{
    QTcpSocket *s = m_server->nextPendingConnection();
    if (!s) return;
    if (m_engine) { // the engine reconnected: the old connection is over
        m_engine->disconnect(this);
        m_engine->abort();
        m_engine->deleteLater();
        if (m_linkUp) emit controlOut("{\"type\":\"sip.close\"}");
    }
    m_engine = s;
    connect(s, SIGNAL(readyRead()), this, SLOT(onEngineData()));
    connect(s, SIGNAL(disconnected()), this, SLOT(onEngineGone()));
    qDebug() << "[SIPRELAY] engine connected";
    m_announced = false;
    if (m_linkUp) {
        emit controlOut("{\"type\":\"sip.open\",\"resume\":false}");
        m_announced = true;
    }
}

void SipRelay::onEngineData()
{
    if (!m_engine) return;
    const QByteArray data = m_engine->readAll();
    // Link down: dropped. The engine's transactions time out and it retries;
    // its registration is resumed when the link comes back (linkUp).
    if (m_linkUp && m_announced && !data.isEmpty()) emit sipOut(data);
}

void SipRelay::onEngineGone()
{
    qDebug() << "[SIPRELAY] engine disconnected";
    if (m_engine) {
        m_engine->deleteLater();
        m_engine = 0;
    }
    if (m_linkUp && m_announced) emit controlOut("{\"type\":\"sip.close\"}");
    m_announced = false;
}

void SipRelay::onRtp()
{
    while (m_udp->hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(m_udp->pendingDatagramSize()));
        QHostAddress from;
        quint16 port = 0;
        if (m_udp->readDatagram(d.data(), d.size(), &from, &port) < 0) continue;
        if (m_linkUp && m_announced && from == QHostAddress::LocalHost) emit rtpOut(port, d);
    }
}

void SipRelay::fromGatewaySip(const QByteArray &bytes)
{
    if (engineConnected()) m_engine->write(bytes);
}

void SipRelay::fromGatewayRtp(quint16 enginePort, const QByteArray &datagram)
{
    if (enginePort) m_udp->writeDatagram(datagram, QHostAddress::LocalHost, enginePort);
}

void SipRelay::gatewayClosedSip()
{
    // The gateway lost its side of the SIP connection. Don't drop the
    // engine's: it would back off for up to ~17 minutes before connecting
    // again. Open a new one at once; the gateway resumes the registration
    // (or the engine renews it within 10 minutes).
    m_announced = false;
    if (m_linkUp && engineConnected()) {
        emit controlOut("{\"type\":\"sip.open\",\"resume\":true}");
        m_announced = true;
    }
}

void SipRelay::linkUp()
{
    m_linkUp = true;
    m_announced = false;
    if (engineConnected()) {
        emit controlOut("{\"type\":\"sip.open\",\"resume\":true}");
        m_announced = true;
    }
}

void SipRelay::linkDown()
{
    m_linkUp = false;
    m_announced = false;
}
