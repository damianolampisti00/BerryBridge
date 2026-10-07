#ifndef CALLAUDIOTEST_HPP_
#define CALLAUDIOTEST_HPP_

#include <QElapsedTimer>
#include <QObject>
#include <QVariantMap>

#include "callaudio.hpp"

class CallLink;
class QFileSystemWatcher;
class QThread;
class QTimer;

// Step 1 of WhatsApp calls (TEST 1, see the call design): call audio inside
// this headless service, no network. Driven by a JSON file dropped into the
// shared data folder (no UI needed while the audio path is being explored):
//
//   /accounts/1000/shared/misc/BerryBridge/calltest.json
//   {"mode": "loopback" | "probe" | "stop", "seconds": 60, "rate": 16000,
//    "fragMs": 20, "jitterMs": 80, "type": "voice", "output": "handset",
//    "capture": "", "playback": ""}
//
// loopback: mic -> ring buffer -> earpiece/speaker (latency, glitches, CPU).
// probe:    1 s noise bursts out, mic level measured with/without them (echo).
// remote:   mic -> gateway (wss, echo mode) -> back -> earpiece/speaker: TEST 2,
//           needs "url" and "token" ("verify": false only to debug TLS).
// remote-net: the same over the network only, with a synthetic 440 Hz tone.
// Results go to calltest.log in the same folder, every 2 s and at the end.
class CallAudioTest : public QObject
{
    Q_OBJECT
public:
    explicit CallAudioTest(QObject *parent = 0);
    ~CallAudioTest();

private slots:
    void onDirChanged();
    void onStats();
    void stopTest();

private:
    void startTest(const QVariantMap &cfg);
    void log(const QString &line);

    QFileSystemWatcher *m_watcher;
    QTimer *m_statsTimer;
    QTimer *m_endTimer;
    AudioRing *m_ring;
    AudioRing *m_downRing;
    QThread *m_netThread;
    CallLink *m_link;
    CallAudioInput *m_input;
    CallAudioOutput *m_output;
    QString m_mode;
    QElapsedTimer m_clock;
    qint64 m_cpuStart;
    int m_lastCaptured;
    int m_lastPlayed;
};

#endif /* CALLAUDIOTEST_HPP_ */
