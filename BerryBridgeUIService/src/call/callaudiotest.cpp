#include "callaudiotest.hpp"
#include "calllink.hpp"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <bb/data/JsonDataAccess>

#include <malloc.h>
#include <math.h>
#include <time.h>

namespace {
const char *const kDir = "/accounts/1000/shared/misc/BerryBridge";
const char *const kCmd = "/accounts/1000/shared/misc/BerryBridge/calltest.json";
const char *const kLog = "/accounts/1000/shared/misc/BerryBridge/calltest.log";

qint64 cpuMs()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

QString dbfs(qint64 sum16, qint64 count)
{
    if (count <= 0) return "-";
    const double meanSquare = double(sum16) * 16.0 / double(count);
    if (meanSquare <= 0) return "-inf";
    return QString::number(10.0 * log10(meanSquare / (32768.0 * 32768.0)), 'f', 1);
}
}

CallAudioTest::CallAudioTest(QObject *parent) :
        QObject(parent),
        m_watcher(new QFileSystemWatcher(this)),
        m_statsTimer(new QTimer(this)),
        m_endTimer(new QTimer(this)),
        m_ring(0), m_downRing(0), m_netThread(0), m_link(0), m_input(0), m_output(0),
        m_cpuStart(0), m_lastCaptured(0), m_lastPlayed(0)
{
    m_watcher->addPath(kDir);
    connect(m_watcher, SIGNAL(directoryChanged(QString)), this, SLOT(onDirChanged()));
    connect(m_statsTimer, SIGNAL(timeout()), this, SLOT(onStats()));
    m_endTimer->setSingleShot(true);
    connect(m_endTimer, SIGNAL(timeout()), this, SLOT(stopTest()));
    onDirChanged(); // a command left from before this start
}

CallAudioTest::~CallAudioTest()
{
    stopTest();
}

void CallAudioTest::log(const QString &line)
{
    QFile f(kLog);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&f) << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " " << line << "\n";
    }
    qDebug() << "[CALLTEST]" << line;
}

void CallAudioTest::onDirChanged()
{
    QFile f(kCmd);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QByteArray json = f.readAll();
    f.close();
    QFile::remove(kCmd);
    bb::data::JsonDataAccess jda;
    const QVariantMap cfg = jda.loadFromBuffer(json).toMap();
    if (jda.hasError()) {
        log("bad calltest.json: " + jda.error().errorMessage());
        return;
    }
    stopTest();
    const QString mode = cfg.value("mode").toString();
    if (mode == "config") {
        QSettings s;
        s.setValue("callServerUrl", cfg.value("url").toString());
        s.setValue("callToken", cfg.value("token").toString());
        s.sync();
        log("call gateway settings saved: " + cfg.value("url").toString());
        emit configChanged();
        return;
    }
    if (mode != "stop") startTest(cfg);
}

void CallAudioTest::startTest(const QVariantMap &c)
{
    CallAudioConfig cfg;
    cfg.rate = c.value("rate", 16000).toInt();
    cfg.fragMs = c.value("fragMs", 20).toInt();
    cfg.jitterMs = c.value("jitterMs", 80).toInt();
    cfg.type = c.value("type", "voice").toString();
    cfg.capture = c.value("capture").toString();
    cfg.playback = c.value("playback").toString();
    cfg.output = c.value("output").toString();
    if (c.value("dump").toBool()) cfg.dumpPrefix = QString(kDir) + "/calltest_";
    m_mode = c.value("mode", "loopback").toString();
    const int seconds = qBound(1, c.value("seconds", 60).toInt(), 900);

    log(QString("START %1 %2s rate=%3 frag=%4ms jitter=%5ms type=%6 output=%7 capture=%8 playback=%9")
        .arg(m_mode).arg(seconds).arg(cfg.rate).arg(cfg.fragMs).arg(cfg.jitterMs).arg(cfg.type)
        .arg(cfg.output.isEmpty() ? "default" : cfg.output)
        .arg(cfg.capture.isEmpty() ? "preferred" : cfg.capture)
        .arg(cfg.playback.isEmpty() ? "preferred" : cfg.playback));

    // Ring: 1 s, far more than the jitter target; the playback side keeps
    // the actual queue near jitterMs.
    m_ring = new AudioRing(cfg.rate * 2);
    if (m_mode.startsWith("remote")) {
        // TEST 2: through the gateway and back, on a network thread of its own.
        const bool withAudio = (m_mode == "remote");
        m_downRing = new AudioRing(cfg.rate * 2);
        m_link = new CallLink(QUrl(c.value("url").toString()), c.value("token").toString().toUtf8(),
                              c.value("verify", true).toBool(), true,
                              withAudio ? m_ring : 0, withAudio ? m_downRing : 0, cfg.rate, !withAudio);
        m_netThread = new QThread(this);
        m_link->moveToThread(m_netThread);
        connect(m_netThread, SIGNAL(started()), m_link, SLOT(start()));
        m_netThread->start(QThread::HighPriority);
        if (withAudio) {
            m_input = new CallAudioInput(cfg, m_ring, this);
            m_output = new CallAudioOutput(cfg, m_downRing, this);
        }
    } else {
        const bool loopback = (m_mode == "loopback");
        m_input = new CallAudioInput(cfg, loopback ? m_ring : 0, this);
        m_output = new CallAudioOutput(cfg, loopback ? m_ring : 0, this);
    }
    if (m_input) m_input->start(QThread::TimeCriticalPriority);
    if (m_output) m_output->start(QThread::TimeCriticalPriority);

    m_clock.start();
    m_cpuStart = cpuMs();
    m_lastCaptured = m_lastPlayed = 0;
    m_statsTimer->start(2000);
    m_endTimer->start(seconds * 1000);
}

void CallAudioTest::onStats()
{
    QString net;
    if (m_link) {
        const int n = qMax(1, int(m_link->rttCount));
        net = QString(" | net %1: sent %2 recv %3 lost %4 reord %5 rtt avg %6 min %7 max %8 ms backlog %9B")
              .arg(m_link->state()).arg(int(m_link->framesSent)).arg(int(m_link->framesReceived))
              .arg(int(m_link->lost)).arg(int(m_link->reordered))
              .arg(int(m_link->rttSumMs) / n).arg(int(m_link->rttCount) ? int(m_link->rttMinMs) : 0)
              .arg(int(m_link->rttMaxMs)).arg(int(m_link->backlogBytes));
    }
    if (!m_input || !m_output) {
        if (m_link) log(QString("t=%1s").arg(m_clock.elapsed() / 1000.0, 0, 'f', 1) + net);
        return;
    }
    const double secs = m_clock.elapsed() / 1000.0;
    const int cap = m_input->bytesCaptured, play = m_output->bytesPlayed;
    const int bytesPerSec = qMax(1, int(m_output->actualRate) * 2);
    struct mallinfo mi = mallinfo();
    QString line = QString("t=%1s cap %2B/s (rate %3 frag %4 ovr %5) | play %6B/s (rate %7 frag %8 undr %9")
            .arg(secs, 0, 'f', 1)
            .arg((cap - m_lastCaptured) / 2).arg(int(m_input->actualRate)).arg(QString("%1x%2").arg(int(m_input->actualFrag)).arg(int(m_input->actualFrags))).arg(int(m_input->overruns))
            .arg((play - m_lastPlayed) / 2).arg(int(m_output->actualRate)).arg(QString("%1x%2").arg(int(m_output->actualFrag)).arg(int(m_output->actualFrags))).arg(int(m_output->underruns));
    line += QString(" conceal %1 drop %2) | ring %3ms lost %4B | cpu %5% heap %6KB")
            .arg(int(m_output->concealed)).arg(int(m_output->dropped))
            .arg(m_ring->level() * 1000 / bytesPerSec).arg(m_ring->overflowBytes())
            .arg(100.0 * (cpuMs() - m_cpuStart) / qMax<qint64>(1, m_clock.elapsed()), 0, 'f', 1)
            .arg((mi.arena + mi.hblkhd) / 1024);
    if (m_mode == "probe") {
        QMutexLocker lock(&m_input->probeMutex);
        line += QString(" | mic with noise %1 dBFS, without %2 dBFS")
                .arg(dbfs(m_input->probeOnSum, m_input->probeOnCount))
                .arg(dbfs(m_input->probeOffSum, m_input->probeOffCount));
    }
    if (!m_input->error().isEmpty()) line += " | CAPTURE ERROR " + m_input->error();
    if (!m_output->error().isEmpty()) line += " | PLAYBACK ERROR " + m_output->error();
    m_lastCaptured = cap;
    m_lastPlayed = play;
    log(line + net);
}

void CallAudioTest::stopTest()
{
    if (!m_input && !m_output && !m_link) return;
    onStats();
    m_statsTimer->stop();
    m_endTimer->stop();
    if (m_input) m_input->requestStop();
    if (m_output) m_output->requestStop();
    if (m_input) m_input->wait(2000);
    if (m_output) m_output->wait(2000);
    if (m_link) {
        QMetaObject::invokeMethod(m_link, "stop", Qt::BlockingQueuedConnection);
        m_netThread->quit();
        m_netThread->wait(3000);
        delete m_link;
        delete m_netThread;
        delete m_downRing;
        m_link = 0;
        m_netThread = 0;
        m_downRing = 0;
    }
    log("STOP " + m_mode);
    delete m_input;
    delete m_output;
    delete m_ring;
    m_input = 0;
    m_output = 0;
    m_ring = 0;
}
