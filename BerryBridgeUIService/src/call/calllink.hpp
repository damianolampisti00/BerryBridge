#ifndef CALLLINK_HPP_
#define CALLLINK_HPP_

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QUrl>

#include "audioring.hpp"

class QTimer;
class WebSocketClient;

// The audio plane of a call between this phone and the gateway
// (WaCalls' bbgateway.go): microphone PCM out, peer PCM in, as binary
// WebSocket frames of [0x01, flags, 0, 0, seq u32 LE, ms u32 LE, PCM...].
// Lives on its own network thread (see CallAudioTest), so neither the
// audio threads nor the service's main thread (Beeper sync, SQLite) can
// delay it, and it can't delay them: the two sides only meet in AudioRings.
class CallLink : public QObject
{
    Q_OBJECT
public:
    // uplink: captured PCM to send (0 = synthetic 440 Hz test tone);
    // downlink: where received PCM goes (0 = discarded, stats only).
    CallLink(const QUrl &url, const QByteArray &token, bool verifyTls, bool echoTest,
             AudioRing *uplink, AudioRing *downlink, int rate, QObject *parent = 0);

    // Stats (read from the controller's thread).
    QAtomicInt framesSent, framesReceived, lost, reordered, rttSumMs, rttCount, rttMinMs, rttMaxMs, backlogBytes;
    QString state() { QMutexLocker l(&m_stateMutex); return m_state; }

public slots:
    void start();
    void stop();

private slots:
    void onOpened();
    void onClosed(const QString &reason);
    void onText(const QByteArray &utf8);
    void onBinary(const QByteArray &data);
    void pump();
    void reconnect();

private:
    void setState(const QString &s) { QMutexLocker l(&m_stateMutex); m_state = s; }

    QUrl m_url;
    QByteArray m_token;
    bool m_verify;
    bool m_echo;
    AudioRing *m_uplink;
    AudioRing *m_downlink;
    int m_rate;
    WebSocketClient *m_ws;
    QTimer *m_pumpTimer;
    QElapsedTimer m_clock;
    quint32 m_seq;
    quint32 m_lastRxSeq;
    qint64 m_syntheticSent;
    bool m_stopping;
    QMutex m_stateMutex;
    QString m_state;
};

#endif /* CALLLINK_HPP_ */
