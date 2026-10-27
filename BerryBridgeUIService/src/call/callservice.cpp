#include "callservice.hpp"
#include "calllink.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <bb/data/JsonDataAccess>
#include <bb/platform/Notification>
#include <bb/platform/NotificationType>
#include <bb/system/InvokeRequest>
#include <bb/system/SystemUiButton>

Q_DECLARE_METATYPE(AudioRing *)

using bb::platform::Notification;

namespace {
const char *const kStateNames[] = { "idle", "incoming", "outgoing", "active", "ended" };

QByteArray toJson(const QVariantMap &map)
{
    bb::data::JsonDataAccess jda;
    QByteArray out; // saveToBuffer appends: always start empty
    jda.saveToBuffer(QVariant(map), &out);
    return out.replace('\n', ' ');
}
}

CallService::CallService(QObject *parent) :
        QObject(parent),
        m_netThread(0), m_link(0),
        m_server(new QLocalServer(this)),
        m_state(Idle), m_since(0), m_muted(false), m_speaker(false), m_answered(false),
        m_upRing(0), m_downRing(0), m_input(0), m_output(0),
        m_ringer(new CallRinger(this)),
        m_ongoing(false),
        m_routeTimer(new QTimer(this)),
        m_statsTimer(new QTimer(this))
{
    qRegisterMetaType<AudioRing *>("AudioRing*");
    connect(m_ringer, SIGNAL(chosen(QString)), this, SLOT(onRingerChosen(QString)));
    // Leftovers of a previous run (a crash mid-call): no call yet.
    QFile::remove(QDir::homePath() + "/call_ringing");
    QFile::remove(QDir::homePath() + "/call_active");

    // Call audio: WaCalls' format (16 kHz mono S16), the voice path of the
    // audio manager (routed and prioritised like a phone call).
    m_audioCfg.rate = 16000;
    m_audioCfg.fragMs = 20;
    m_audioCfg.jitterMs = 120; // the server sends 60 ms frames
    m_audioCfg.type = "voice";
    m_audioCfg.output = "handset";
    m_audioCfg.input = "handset";
    // During a call: a Bluetooth headset or a wired one plugged in or out
    // moves the call there, as on the phone app.
    m_routeTimer->setInterval(1000);
    connect(m_routeTimer, SIGNAL(timeout()), this, SLOT(checkRoute()));
    m_statsTimer->setInterval(10000);
    connect(m_statsTimer, SIGNAL(timeout()), this, SLOT(logStats()));

    const QString path = socketPath();
    QLocalServer::removeServer(path);
    if (!m_server->listen(path)) qWarning() << "[CALL] local server:" << m_server->errorString();
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewClient()));

    m_linkState = "not configured";
    reloadConfig();
}

CallService::~CallService()
{
    stopAudio();
    stopLink();
}

QString CallService::socketPath()
{
    // The app's own data folder: shared by the UI and this service, out of
    // reach of every other app.
    return QDir::homePath() + "/callctl.sock";
}

// --- gateway link ----------------------------------------------------------

void CallService::stopLink()
{
    if (!m_link) return;
    QMetaObject::invokeMethod(m_link, "stop", Qt::BlockingQueuedConnection);
    m_netThread->quit();
    m_netThread->wait(3000);
    delete m_link;
    delete m_netThread;
    m_link = 0;
    m_netThread = 0;
}

void CallService::reloadConfig()
{
    m_settings.sync();
    const QString url = m_settings.value("callServerUrl").toString().trimmed();
    const QString token = m_settings.value("callToken").toString().trimmed();
    // Unchanged settings (Settings re-saves them on every visit): keep the
    // link -- above all during a call.
    if (m_link && url == m_linkUrl && token == m_linkToken) return;
    m_linkUrl = url;
    m_linkToken = token;
    stopLink();
    if (url.isEmpty() || token.isEmpty()) {
        m_linkState = "not configured";
        broadcast();
        return;
    }
    m_linkState = "connecting";
    m_link = new CallLink(QUrl(url), token.toUtf8(), true, false, 0, 0, m_audioCfg.rate);
    m_netThread = new QThread(this);
    m_link->moveToThread(m_netThread);
    connect(m_netThread, SIGNAL(started()), m_link, SLOT(start()));
    connect(m_link, SIGNAL(linkOpened()), this, SLOT(onLinkOpened()));
    connect(m_link, SIGNAL(linkClosed(QString)), this, SLOT(onLinkClosed(QString)));
    connect(m_link, SIGNAL(textReceived(QByteArray)), this, SLOT(onGatewayText(QByteArray)));
    m_netThread->start(QThread::HighPriority);
    if (m_upRing) QMetaObject::invokeMethod(m_link, "setAudio", Qt::QueuedConnection,
                                            Q_ARG(AudioRing*, m_upRing), Q_ARG(AudioRing*, m_downRing));
    broadcast();
}

void CallService::onLinkOpened()
{
    m_linkState = "open";
    qDebug() << "[CALL] gateway link open";
    broadcast();
}

void CallService::onLinkClosed(const QString &reason)
{
    m_linkState = "closed: " + reason;
    qWarning() << "[CALL] gateway link closed:" << reason;
    // A call in progress survives a short reconnect: the gateway keeps
    // routing its audio to whichever connection is current.
    broadcast();
}

void CallService::sendGateway(const QVariantMap &msg)
{
    if (!m_link) return;
    QMetaObject::invokeMethod(m_link, "sendText", Qt::QueuedConnection, Q_ARG(QByteArray, toJson(msg)));
}

void CallService::onGatewayText(const QByteArray &utf8)
{
    bb::data::JsonDataAccess jda;
    const QVariantMap ev = jda.loadFromBuffer(utf8).toMap();
    const QString type = ev.value("type").toString();
    const QString id = ev.value("callId").toString();
    qDebug() << "[CALL] <<" << utf8.left(300);

    if (type == "hello") {
        const QVariantList calls = ev.value("calls").toList();
        // Our call ended while the link was down (its call.ended never came):
        // seen 2026-10-15, an incoming call hung up by the caller the moment
        // the link dropped left the phone "in a call" (and installs blocked)
        // for an hour.
        if (m_state != Idle && m_state != Ended && !m_callId.isEmpty()) {
            bool known = false;
            for (int i = 0; i < calls.size() && !known; ++i)
                known = calls.at(i).toMap().value("callId").toString() == m_callId;
            if (!known) {
                qDebug() << "[CALL] call" << m_callId << "ended while the link was down";
                const bool missed = (m_state == Incoming);
                ring(false);
                stopAudio();
                setState(Ended, "ended");
                if (missed) {
                    postHubNotification("Chiamata WhatsApp persa", m_name.isEmpty() ? m_phone : m_name,
                                        "call-missed-" + m_callId);
                }
            }
        }
        // After a service restart mid-call: pick the call back up.
        for (int i = 0; i < calls.size() && m_state == Idle; ++i) {
            const QVariantMap c = calls.at(i).toMap();
            m_callId = c.value("callId").toString();
            m_phone = c.value("phone").toString();
            m_name = nameForPhone(m_phone);
            if (c.value("state").toString() == "connected") {
                m_answered = true;
                m_since = QDateTime::currentMSecsSinceEpoch();
                startAudio();
                setState(Active);
            } else if (c.value("direction").toString() == "inbound") {
                setState(Incoming);
                ring(true);
            }
        }
    } else if (type == "call.incoming") {
        if (m_state != Idle && m_state != Ended) return; // already on a call: it rings on the main phone only
        m_callId = id;
        m_phone = ev.value("phone").toString();
        m_name = nameForPhone(m_phone);
        m_muted = m_speaker = m_answered = false;
        m_since = 0;
        m_reason.clear();
        setState(Incoming);
        ring(true); // also brings the call screen to the front
    } else if (type == "call.started") {
        if (m_state == Outgoing) m_callId = id;
    } else if (type == "call.state") {
        if (id != m_callId) return;
        if (ev.value("state").toString() == "connected" && m_state != Active) {
            ring(false);
            m_since = QDateTime::currentMSecsSinceEpoch();
            startAudio();
            setState(Active);
        } else if (ev.value("state").toString() == "connected" && m_since == 0) {
            m_since = QDateTime::currentMSecsSinceEpoch();
            broadcast();
        }
    } else if (type == "call.ended") {
        if (id != m_callId || m_state == Idle || m_state == Ended) return;
        const bool missed = (m_state == Incoming);
        ring(false);
        stopAudio();
        setState(Ended, ev.value("reason").toString());
        if (missed) {
            postHubNotification("Chiamata WhatsApp persa", m_name.isEmpty() ? m_phone : m_name,
                                "call-missed-" + m_callId);
        }
    } else if (type == "error") {
        qWarning() << "[CALL] gateway error for" << ev.value("for").toString() << ev.value("message").toString();
        if (m_state == Outgoing && ev.value("for").toString() == "call.start") {
            stopAudio();
            setState(Ended, ev.value("message").toString());
        }
    }
}

// --- UI clients ------------------------------------------------------------

void CallService::onNewClient()
{
    while (QLocalSocket *s = m_server->nextPendingConnection()) {
        m_clients << s;
        connect(s, SIGNAL(readyRead()), this, SLOT(onClientData()));
        connect(s, SIGNAL(disconnected()), this, SLOT(onClientGone()));
        s->write(toJson(statusMap()) + "\n");
    }
    emit uiClientsChanged(m_clients.size());
}

void CallService::onClientGone()
{
    QLocalSocket *s = qobject_cast<QLocalSocket *>(sender());
    m_clients.removeAll(s);
    if (s) s->deleteLater();
    emit uiClientsChanged(m_clients.size());
}

void CallService::onClientData()
{
    QLocalSocket *s = qobject_cast<QLocalSocket *>(sender());
    if (!s) return;
    if (!s->canReadLine() && s->bytesAvailable() > 64 * 1024) { // no command is that long
        s->abort();
        return;
    }
    while (s->canReadLine()) {
        bb::data::JsonDataAccess jda;
        const QVariantMap cmd = jda.loadFromBuffer(s->readLine().trimmed()).toMap();
        if (cmd.value("cmd").toString() == "status") s->write(toJson(statusMap()) + "\n");
        else handleCommand(cmd);
    }
}

void CallService::handleCommand(const QVariantMap &cmd)
{
    const QString c = cmd.value("cmd").toString();
    if (c == "config") {
        reloadConfig();
    } else if (c == "start") {
        if (m_state != Idle && m_state != Ended) return;
        m_phone = cmd.value("phone").toString().trimmed();
        m_name = cmd.value("name").toString();
        if (m_name.isEmpty()) m_name = nameForPhone(m_phone);
        m_callId.clear();
        m_muted = m_speaker = false;
        m_answered = true;
        m_since = 0;
        m_reason.clear();
        if (m_linkState != "open") {
            setState(Ended, "Server chiamate non raggiungibile");
            return;
        }
        QVariantMap msg;
        msg["type"] = "call.start";
        msg["phone"] = m_phone;
        sendGateway(msg);
        setState(Outgoing);
    } else if (c == "accept") {
        if (m_state != Incoming) return;
        ring(false);
        m_answered = true;
        QVariantMap msg;
        msg["type"] = "call.accept";
        msg["callId"] = m_callId;
        sendGateway(msg);
        startAudio(); // ready for the first audio; the timer starts on "connected"
        setState(Active);
    } else if (c == "reject") {
        if (m_state != Incoming) return;
        ring(false);
        QVariantMap msg;
        msg["type"] = "call.reject";
        msg["callId"] = m_callId;
        sendGateway(msg);
        setState(Ended, "declined");
    } else if (c == "hangup") {
        if (m_state != Outgoing && m_state != Active) return;
        QVariantMap msg;
        msg["type"] = "call.hangup";
        msg["callId"] = m_callId;
        if (!m_callId.isEmpty()) sendGateway(msg);
        stopAudio();
        setState(Ended, "user_ended");
    } else if (c == "mute") {
        m_muted = cmd.value("on").toBool();
        QVariantMap msg;
        msg["type"] = "call.mute";
        msg["muted"] = m_muted;
        sendGateway(msg);
        broadcast();
    } else if (c == "speaker") {
        m_speaker = cmd.value("on").toBool();
        checkRoute();
        broadcast();
    }
}

QVariantMap CallService::statusMap() const
{
    QVariantMap m;
    m["event"] = "state";
    m["state"] = kStateNames[m_state];
    m["callId"] = m_callId;
    m["phone"] = m_phone;
    m["name"] = m_name;
    m["since"] = m_since;
    m["muted"] = m_muted;
    m["speaker"] = m_speaker;
    m["route"] = m_audioCfg.output; // handset speaker btsco headset headphone
    m["reason"] = m_reason;
    m["link"] = m_linkState;
    return m;
}

void CallService::broadcast()
{
    const QByteArray line = toJson(statusMap()) + "\n";
    for (int i = 0; i < m_clients.size(); ++i) m_clients.at(i)->write(line);
}

void CallService::setState(State s, const QString &reason)
{
    m_state = s;
    if (!reason.isEmpty() || s != Ended) m_reason = reason;
    qDebug() << "[CALL] state" << kStateNames[s] << reason;
    // Read by the UI at start (it must not clear the ringing's effects).
    QFile flag(QDir::homePath() + "/call_ringing");
    if (s == Incoming) {
        if (flag.open(QIODevice::WriteOnly)) flag.close();
    } else {
        flag.remove();
    }
    // A call going on: package.ps1 -Install won't update the app (that stops
    // this service, and the call with it).
    QFile busy(QDir::homePath() + "/call_active");
    if (s == Incoming || s == Outgoing || s == Active) {
        if (busy.open(QIODevice::WriteOnly)) busy.close();
    } else {
        busy.remove();
    }
    // A call in progress keeps a quiet Hub notification, like the phone's:
    // tapping it brings back the call screen.
    if (s == Outgoing || s == Active) {
        if (!m_ongoing) {
            m_ongoing = true;
            postHubNotification("Chiamata WhatsApp in corso", m_name.isEmpty() ? m_phone : m_name,
                                "call-ongoing", true);
        }
    } else if (m_ongoing) {
        m_ongoing = false;
        Notification::deleteFromInbox("call-ongoing");
    }
    broadcast();
    if (s == Ended) QTimer::singleShot(4000, this, SLOT(finishEnded()));
}

void CallService::keepAlive()
{
    if (m_link) QMetaObject::invokeMethod(m_link, "keepAlive", Qt::QueuedConnection);
}

void CallService::finishEnded()
{
    if (m_state != Ended) return;
    m_callId.clear();
    m_since = 0;
    setState(Idle);
}

// --- audio -------------------------------------------------------------------

// The call's devices: the speaker when asked for; otherwise a Bluetooth
// headset (hands-free profile), then a wired headset or headphones, then the
// earpiece -- read from the audio service's device objects.
namespace {
bool deviceConnected(const char *name)
{
    QFile f(QString("/pps/services/audio/devices/") + name);
    return f.open(QIODevice::ReadOnly) && f.readAll().contains("connected:b:true");
}
}

void CallService::checkRoute()
{
    QString out = "handset", in = "handset";
    if (m_speaker) {
        out = "speaker";
    } else if (deviceConnected("btsco")) {
        out = in = "btsco";
    } else if (deviceConnected("headset")) {
        out = in = "headset";
    } else if (deviceConnected("headphone")) {
        out = "headphone";
    }
    if (out == m_audioCfg.output && in == m_audioCfg.input) return;
    qDebug() << "[CALL] route" << out << in;
    m_audioCfg.output = out;
    m_audioCfg.input = in;
    if (!m_input) return; // used by the next startAudio()
    // Reopen both directions on the new devices; the rings and the network
    // link stay as they are.
    m_input->requestStop();
    m_input->wait(2000);
    delete m_input;
    m_input = new CallAudioInput(m_audioCfg, m_upRing, this);
    m_input->start(QThread::TimeCriticalPriority);
    restartOutput();
}

void CallService::startAudio()
{
    if (m_input) return;
    checkRoute();
    m_routeTimer->start();
    m_upRing = new AudioRing(m_audioCfg.rate * 2);   // 1 s
    m_downRing = new AudioRing(m_audioCfg.rate * 2);
    m_input = new CallAudioInput(m_audioCfg, m_upRing, this);
    m_output = new CallAudioOutput(m_audioCfg, m_downRing, this);
    m_input->start(QThread::TimeCriticalPriority);
    m_output->start(QThread::TimeCriticalPriority);
    if (m_link) QMetaObject::invokeMethod(m_link, "setAudio", Qt::QueuedConnection,
                                          Q_ARG(AudioRing*, m_upRing), Q_ARG(AudioRing*, m_downRing));
    m_statsLast.clear();
    collectStats(false); // the link's counters run across calls: start from here
    m_statsTimer->start();
}

void CallService::stopAudio()
{
    m_routeTimer->stop();
    if (!m_input && !m_output) return;
    m_statsTimer->stop();
    collectStats(true); // the call's last seconds
    // Detach from the network thread FIRST (blocking), so it never touches
    // the rings after they are freed.
    if (m_link) QMetaObject::invokeMethod(m_link, "setAudio", Qt::BlockingQueuedConnection,
                                          Q_ARG(AudioRing*, (AudioRing *)0), Q_ARG(AudioRing*, (AudioRing *)0));
    if (m_input) { m_input->requestStop(); m_input->wait(2000); delete m_input; m_input = 0; }
    if (m_output) { m_output->requestStop(); m_output->wait(2000); delete m_output; m_output = 0; }
    delete m_upRing;
    delete m_downRing;
    m_upRing = m_downRing = 0;
}

// Every 10 s of a call, what the audio path did meanwhile, to tell the
// causes of crackles apart: frames lost or late on the network (rtt, the
// sender's backlog), playback with nothing to play (concealed blocks of
// silence, device underruns), audio thrown away to bound the latency
// (drop) or because a buffer was full (overflow), mic overruns.
void CallService::logStats()
{
    collectStats(true);
}

void CallService::collectStats(bool print)
{
    QString line;
    if (m_link) {
        const qint64 n = statDelta("rttN", m_link->rttCount);
        const qint64 sum = statDelta("rttSum", m_link->rttSumMs);
        line += QString(" net recv %1 sent %2 lost %3 reord %4 rtt %5ms backlog %6B")
                .arg(statDelta("recv", m_link->framesReceived)).arg(statDelta("sent", m_link->framesSent))
                .arg(statDelta("lost", m_link->lost)).arg(statDelta("reord", m_link->reordered))
                .arg(n > 0 ? sum / n : -1).arg(int(m_link->backlogBytes));
    }
    if (m_output && m_downRing) {
        line += QString(" | play conceal %1 rebuf %7 undr %2 drop %3 buffered %4ms overflow %5B")
                .arg(statDelta("conceal", m_output->concealed)).arg(statDelta("undr", m_output->underruns))
                .arg(statDelta("drop", m_output->dropped))
                .arg(m_downRing->level() * 1000 / qMax(1, m_audioCfg.rate * 2))
                .arg(statDelta("downOverflow", m_downRing->overflowBytes()))
                .arg(statDelta("rebuf", m_output->rebuffers));
    }
    if (m_input && m_upRing) {
        line += QString(" | mic ovr %1 overflow %2B")
                .arg(statDelta("ovr", m_input->overruns)).arg(statDelta("upOverflow", m_upRing->overflowBytes()));
    }
    if (print && !line.isEmpty()) qDebug() << ("[CALL] stats" + line);
}

// How much a counter grew since the last stats line. Counters of a reopened
// device (route change) start again from 0: count from there.
qint64 CallService::statDelta(const char *key, qint64 now)
{
    const qint64 last = m_statsLast.value(key, 0);
    m_statsLast.insert(key, now);
    return now >= last ? now - last : now;
}

void CallService::restartOutput()
{
    if (!m_output) return;
    m_output->requestStop();
    m_output->wait(2000);
    delete m_output;
    m_output = new CallAudioOutput(m_audioCfg, m_downRing, this);
    m_output->start(QThread::TimeCriticalPriority);
}

// --- ringing, notifications, UI ----------------------------------------------

void CallService::ring(bool on)
{
    if (!on) {
        m_ringer->stop();
        Notification::deleteFromInbox("call-ringing");
        return;
    }
    if (m_ringer->isRinging()) return;
    const QString who = m_name.isEmpty() ? (m_phone.isEmpty() ? QString("Numero sconosciuto") : m_phone) : m_name;
    // The Hub item first and quiet: a notification with effects posted after
    // the dialog replaced the dialog's repeating ring/vibration after one buzz.
    postHubNotification("Chiamata WhatsApp in arrivo", who, "call-ringing", true);
    // The system dialog with Rispondi/Rifiuta, also on the lock screen.
    m_ringer->ring("Chiamata WhatsApp", who);
    m_ringer->wakeScreen("Chiamata WhatsApp da " + who);
    // Like a phone app, the call screen comes to the front -- once the screen
    // is on (with the screen off the navigator starts it in the background).
    QTimer::singleShot(1500, this, SLOT(bringUpCallScreen()));
}

void CallService::bringUpCallScreen()
{
    if (m_state == Incoming) m_ringer->invokeItem("call-ringing");
}

void CallService::onRingerChosen(const QString &context)
{
    if (m_state != Incoming) return;
    QVariantMap cmd;
    cmd["cmd"] = (context == "answer") ? "accept" : "reject";
    handleCommand(cmd);
    // Answered from the dialog: the call screen comes to the front (through
    // the "in corso" notification, posted by setState(Active)).
    if (context == "answer") m_ringer->invokeItem("call-ongoing");
}

void CallService::postHubNotification(const QString &title, const QString &body, const QString &key,
                                      bool quiet)
{
    Notification *n = new Notification(key, this);
    if (quiet) n->setType(bb::platform::NotificationType::AllAlertsOff);
    n->setTitle(title);
    n->setBody(body);
    bb::system::InvokeRequest req;
    req.setTarget("it.berrybridge.ui");
    req.setAction("bb.action.START");
    n->setInvokeRequest(req);
    n->notify();
    n->deleteLater();
}


QString CallService::nameForPhone(const QString &phone) const
{
    QString digits;
    for (int i = 0; i < phone.size(); ++i) if (phone.at(i).isDigit()) digits += phone.at(i);
    if (digits.size() < 6) return QString();
    QSqlDatabase db = QSqlDatabase::database("chats_db_conn");
    if (!db.isOpen()) return QString();
    QSqlQuery q(db);
    q.prepare("SELECT title FROM chats WHERE type = 'single' AND participants_json LIKE ? LIMIT 1");
    q.bindValue(0, "%" + digits + "%");
    if (q.exec() && q.next()) return q.value(0).toString();
    return QString();
}
