#include "calllink.hpp"
#include "websocketclient.hpp"

#include <QDebug>
#include <QTimer>
#include <QtEndian>

#include <math.h>

namespace {
const int kHeader = 12;
const int kFrameMs = 20;          // uplink block: 20 ms (the server re-blocks to MLow's 60 ms)
const qint64 kMaxBacklog = 32000; // ~1 s of audio waiting in the socket: drop instead of queueing more
}

CallLink::CallLink(const QUrl &url, const QByteArray &token, bool verifyTls, bool echoTest,
                   AudioRing *uplink, AudioRing *downlink, int rate, QObject *parent) :
        QObject(parent),
        rttMinMs(1 << 30),
        m_url(url), m_token(token), m_verify(verifyTls), m_echo(echoTest),
        m_uplink(uplink), m_downlink(downlink), m_rate(rate),
        m_ws(0), m_pumpTimer(0), m_seq(0), m_lastRxSeq(0), m_syntheticSent(0), m_stopping(false)
{
}

void CallLink::start()
{
    // Created here, in the network thread, so the socket lives there.
    m_ws = new WebSocketClient(this);
    connect(m_ws, SIGNAL(opened()), this, SLOT(onOpened()));
    connect(m_ws, SIGNAL(closed(QString)), this, SLOT(onClosed(QString)));
    connect(m_ws, SIGNAL(textMessage(QByteArray)), this, SLOT(onText(QByteArray)));
    connect(m_ws, SIGNAL(binaryMessage(QByteArray)), this, SLOT(onBinary(QByteArray)));
    m_pumpTimer = new QTimer(this);
    m_pumpTimer->setInterval(10);
    connect(m_pumpTimer, SIGNAL(timeout()), this, SLOT(pump()));
    m_clock.start();
    reconnect();
}

void CallLink::reconnect()
{
    if (m_stopping) return;
    setState("connecting");
    WebSocketClient::Headers h;
    h << qMakePair(QByteArray("Authorization"), "Bearer " + m_token);
    h << qMakePair(QByteArray("User-Agent"), QByteArray("BerryBridge-BB10/2"));
    m_ws->open(m_url, h, m_verify);
}

void CallLink::stop()
{
    m_stopping = true;
    if (m_pumpTimer) m_pumpTimer->stop();
    if (m_ws) m_ws->close();
}

void CallLink::onOpened()
{
    setState("open" + (m_ws->tlsReport().isEmpty() ? QString() : " (TLS: " + m_ws->tlsReport() + ")"));
    if (m_echo) m_ws->sendText("{\"type\":\"test.echo\",\"on\":true}");
    m_syntheticSent = m_clock.elapsed() * m_rate / 1000; // tone starts now, no catch-up burst
    if (m_uplink) m_uplink->clear();                       // nor stale mic audio
    m_pumpTimer->start();
}

void CallLink::onClosed(const QString &reason)
{
    setState("closed: " + reason);
    if (m_pumpTimer) m_pumpTimer->stop();
    qWarning() << "[CALLLINK] closed:" << reason;
    if (!m_stopping) QTimer::singleShot(2000, this, SLOT(reconnect()));
}

void CallLink::onText(const QByteArray &utf8)
{
    qDebug() << "[CALLLINK] <<" << utf8.left(200);
}

void CallLink::onBinary(const QByteArray &data)
{
    if (data.size() < kHeader || uchar(data.at(0)) != 0x01) return;
    const uchar *h = reinterpret_cast<const uchar *>(data.constData());
    const quint32 seq = qFromLittleEndian<quint32>(h + 4);
    const quint32 ts = qFromLittleEndian<quint32>(h + 8);
    framesReceived.ref();
    if (m_lastRxSeq && seq != m_lastRxSeq + 1) {
        if (seq > m_lastRxSeq) lost.fetchAndAddRelaxed(int(seq - m_lastRxSeq - 1));
        else reordered.ref();
    }
    if (seq > m_lastRxSeq) m_lastRxSeq = seq;
    if (m_echo) { // our own frame back: its timestamp is our clock
        const int rtt = int(quint32(m_clock.elapsed()) - ts);
        rttSumMs.fetchAndAddRelaxed(rtt);
        rttCount.ref();
        if (rtt < int(rttMinMs)) rttMinMs = rtt;
        if (rtt > int(rttMaxMs)) rttMaxMs = rtt;
    }
    if (m_downlink) m_downlink->write(data.constData() + kHeader, data.size() - kHeader);
}

void CallLink::pump()
{
    if (!m_ws || !m_ws->isOpen()) return;
    const int frameBytes = m_rate * 2 * kFrameMs / 1000;
    backlogBytes = int(m_ws->bytesToWrite());
    for (;;) {
        QByteArray frame(kHeader + frameBytes, 0);
        if (m_uplink) {
            if (m_uplink->level() < frameBytes) break;
            m_uplink->read(frame.data() + kHeader, frameBytes);
        } else {
            // Synthetic source: 440 Hz at -12 dBFS, paced by the clock.
            const qint64 due = m_clock.elapsed() * m_rate / 1000;
            if (due - m_syntheticSent < frameBytes / 2) break;
            qint16 *s = reinterpret_cast<qint16 *>(frame.data() + kHeader);
            for (int i = 0; i < frameBytes / 2; ++i) {
                s[i] = qint16(8192.0 * sin(2.0 * 3.14159265358979 * 440.0 * double(m_syntheticSent + i) / m_rate));
            }
            m_syntheticSent += frameBytes / 2;
        }
        if (m_ws->bytesToWrite() > kMaxBacklog) continue; // network stalled: drop, don't pile up
        uchar *h = reinterpret_cast<uchar *>(frame.data());
        h[0] = 0x01;
        qToLittleEndian<quint32>(++m_seq, h + 4);
        qToLittleEndian<quint32>(quint32(m_clock.elapsed()), h + 8);
        if (m_ws->sendBinary(frame)) framesSent.ref();
    }
}
