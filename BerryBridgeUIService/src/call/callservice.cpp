#include "callservice.hpp"
#include "calllink.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <bb/data/JsonDataAccess>
#include <bb/platform/Notification>
#include <bb/platform/NotificationDialog>
#include <bb/platform/NotificationResult>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>
#include <bb/system/SystemUiButton>

Q_DECLARE_METATYPE(AudioRing *)

using bb::platform::Notification;
using bb::platform::NotificationDialog;

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
        m_ringDialog(0),
        m_invoke(new bb::system::InvokeManager(this))
{
    qRegisterMetaType<AudioRing *>("AudioRing*");

    // Call audio: WaCalls' format (16 kHz mono S16), the voice path of the
    // audio manager (routed and prioritised like a phone call).
    m_audioCfg.rate = 16000;
    m_audioCfg.fragMs = 20;
    m_audioCfg.jitterMs = 120; // the server sends 60 ms frames
    m_audioCfg.type = "voice";
    m_audioCfg.output = "handset";

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
        // After a service restart mid-call: pick the call back up.
        const QVariantList calls = ev.value("calls").toList();
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
        ring(true);
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
}

void CallService::onClientGone()
{
    QLocalSocket *s = qobject_cast<QLocalSocket *>(sender());
    m_clients.removeAll(s);
    if (s) s->deleteLater();
}

void CallService::onClientData()
{
    QLocalSocket *s = qobject_cast<QLocalSocket *>(sender());
    if (!s) return;
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
        m_audioCfg.output = m_speaker ? "speaker" : "handset";
        restartOutput();
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
    broadcast();
    if (s == Ended) QTimer::singleShot(4000, this, SLOT(finishEnded()));
}

void CallService::finishEnded()
{
    if (m_state != Ended) return;
    m_callId.clear();
    m_since = 0;
    setState(Idle);
}

// --- audio -------------------------------------------------------------------

void CallService::startAudio()
{
    if (m_input) return;
    m_upRing = new AudioRing(m_audioCfg.rate * 2);   // 1 s
    m_downRing = new AudioRing(m_audioCfg.rate * 2);
    m_input = new CallAudioInput(m_audioCfg, m_upRing, this);
    m_output = new CallAudioOutput(m_audioCfg, m_downRing, this);
    m_input->start(QThread::TimeCriticalPriority);
    m_output->start(QThread::TimeCriticalPriority);
    if (m_link) QMetaObject::invokeMethod(m_link, "setAudio", Qt::QueuedConnection,
                                          Q_ARG(AudioRing*, m_upRing), Q_ARG(AudioRing*, m_downRing));
}

void CallService::stopAudio()
{
    if (!m_input && !m_output) return;
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
        if (m_ringDialog) {
            m_ringDialog->disconnect(this);
            m_ringDialog->cancel();
            m_ringDialog->deleteLater();
            m_ringDialog = 0;
        }
        Notification::deleteFromInbox("call-ringing");
        return;
    }
    if (m_ringDialog) return;
    const QString who = m_name.isEmpty() ? (m_phone.isEmpty() ? QString("Numero sconosciuto") : m_phone) : m_name;
    m_ringDialog = new NotificationDialog(this);
    m_ringDialog->setTitle("Chiamata WhatsApp");
    m_ringDialog->setBody(who);
    m_ringDialog->setRepeat(true); // keeps ringing/vibrating until answered
    m_ringDialog->appendButton(new bb::system::SystemUiButton("Rispondi", m_ringDialog));
    m_ringDialog->appendButton(new bb::system::SystemUiButton("Rifiuta", m_ringDialog));
    connect(m_ringDialog, SIGNAL(finished(bb::platform::NotificationResult::Type)), this, SLOT(onRingDialogFinished()));
    m_ringDialog->show();
    postHubNotification("Chiamata WhatsApp in arrivo", who, "call-ringing");
}

void CallService::onRingDialogFinished()
{
    NotificationDialog *dialog = qobject_cast<NotificationDialog *>(sender());
    if (!dialog || dialog != m_ringDialog) return;
    bb::system::SystemUiButton *b = dialog->buttonSelection();
    const QString label = b ? b->label() : QString();
    m_ringDialog = 0;
    dialog->deleteLater();
    QVariantMap cmd;
    if (label == "Rispondi") {
        cmd["cmd"] = "accept";
        handleCommand(cmd);
        openUi();
    } else if (label == "Rifiuta") {
        cmd["cmd"] = "reject";
        handleCommand(cmd);
    } else {
        openUi(); // dismissed some other way: show the call screen to decide there
    }
}

void CallService::postHubNotification(const QString &title, const QString &body, const QString &key)
{
    Notification *n = new Notification(key, this);
    n->setTitle(title);
    n->setBody(body);
    bb::system::InvokeRequest req;
    req.setTarget("it.berrybridge.ui");
    req.setAction("bb.action.START");
    n->setInvokeRequest(req);
    n->notify();
    n->deleteLater();
}

void CallService::openUi()
{
    // The UI opens the call screen by itself once its CallClient connects
    // and sees a call in progress.
    bb::system::InvokeRequest req;
    req.setTarget("it.berrybridge.ui");
    req.setAction("bb.action.START");
    m_invoke->invoke(req);
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
