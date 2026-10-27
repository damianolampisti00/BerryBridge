#include "../../../BerryBridgeUIService/src/cachepaths.hpp"
#include "callclient.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QtNetwork/QLocalSocket>
#include <bb/data/JsonDataAccess>
#include <bb/data/SqlDataAccess>

namespace {
static QString chatsDb() { return bbCachePath("chats.db"); }

QVariant parseJson(const QString &text)
{
    QString t = text.trimmed();
    // Old rows may start with a stray "[]"/"{}" (saveToBuffer appends, see Database.cpp).
    if (t.startsWith("[]") || t.startsWith("{}")) t = t.mid(2);
    bb::data::JsonDataAccess jda;
    return jda.loadFromBuffer(t.toUtf8());
}
}

CallClient::CallClient(QObject *parent) :
        QObject(parent),
        m_socket(new QLocalSocket(this))
{
    connect(m_socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_socket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(m_socket, SIGNAL(connected()), &m_retry, SLOT(stop()));
    connect(m_socket, SIGNAL(error(QLocalSocket::LocalSocketError)), this, SLOT(onDisconnected()));
    m_retry.setInterval(2000);
    connect(&m_retry, SIGNAL(timeout()), this, SLOT(connectToService()));
    // The call timer's tick: only during a call (it woke the phone every
    // second for as long as the app was open). See updateTicker().
    m_ticker.setInterval(1000);
    connect(&m_ticker, SIGNAL(timeout()), this, SIGNAL(tick()));
    connect(this, SIGNAL(changed()), this, SLOT(updateTicker()));
    connectToService();
}

void CallClient::updateTicker()
{
    const QString s = state();
    const bool live = !s.isEmpty() && s != "idle";
    if (live && !m_ticker.isActive()) {
        m_ticker.start();
        emit tick();
    } else if (!live && m_ticker.isActive()) {
        m_ticker.stop();
    }
}

void CallClient::connectToService()
{
    if (m_socket->state() != QLocalSocket::UnconnectedState) return;
    // The service's socket (CallService::socketPath): same app, same data
    // folder. Asynchronous: never blocks the UI thread (startup included).
    m_socket->connectToServer(QDir::homePath() + "/callctl.sock");
    // A local socket usually connects inside connectToServer(), connected()
    // included: restarting the retry after that kept it firing every 2 s
    // for as long as the app was open (28 wake-ups a minute when minimized).
    if (m_socket->state() != QLocalSocket::ConnectedState && !m_retry.isActive()) m_retry.start();
}

void CallClient::onDisconnected()
{
    if (m_socket->state() != QLocalSocket::UnconnectedState) m_socket->abort();
    m_status.clear();
    emit changed();
    if (!m_retry.isActive()) m_retry.start();
}

void CallClient::onReadyRead()
{
    while (m_socket->canReadLine()) {
        bb::data::JsonDataAccess jda;
        const QVariantMap ev = jda.loadFromBuffer(m_socket->readLine().trimmed()).toMap();
        if (ev.value("event").toString() != "state") continue;
        const QString before = state();
        m_status = ev;
        emit changed();
        emit tick();
        const QString now = state();
        if (now != before && (now == "incoming" || now == "outgoing" || now == "active")) emit callScreenNeeded();
    }
}

void CallClient::send(const QVariantMap &cmd)
{
    if (m_socket->state() != QLocalSocket::ConnectedState) {
        qWarning() << "[CALL] service not reachable";
        connectToService();
        return;
    }
    bb::data::JsonDataAccess jda;
    QByteArray out;
    jda.saveToBuffer(QVariant(cmd), &out);
    m_socket->write(out.replace('\n', ' ') + "\n");
    m_socket->flush();
}

QString CallClient::elapsed() const
{
    const qint64 since = m_status.value("since").toLongLong();
    if (since <= 0) return QString();
    const qint64 s = qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - since) / 1000);
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

void CallClient::startCall(const QString &phone, const QString &name)
{
    QVariantMap c;
    c["cmd"] = "start";
    c["phone"] = phone;
    c["name"] = name;
    send(c);
}

void CallClient::accept() { QVariantMap c; c["cmd"] = "accept"; send(c); }
void CallClient::reject() { QVariantMap c; c["cmd"] = "reject"; send(c); }
void CallClient::hangup() { QVariantMap c; c["cmd"] = "hangup"; send(c); }
void CallClient::reloadConfig() { QVariantMap c; c["cmd"] = "config"; send(c); }

void CallClient::setMuted(bool on)
{
    QVariantMap c;
    c["cmd"] = "mute";
    c["on"] = on;
    send(c);
}

void CallClient::setSpeaker(bool on)
{
    QVariantMap c;
    c["cmd"] = "speaker";
    c["on"] = on;
    send(c);
}

bool CallClient::isWhatsAppAccount(const QString &accountID) const
{
    return accountID.contains("whatsapp", Qt::CaseInsensitive);
}

QString CallClient::phoneForChat(const QString &chatID) const
{
    bb::data::SqlDataAccess sda(chatsDb());
    QVariantMap params;
    params["id"] = chatID;
    const QVariantList rows = sda.execute("SELECT participants_json FROM chats WHERE id = :id LIMIT 1", params).toList();
    if (rows.isEmpty()) return QString();
    const QVariant parsed = parseJson(rows.first().toMap().value("participants_json").toString());
    QVariantList items = parsed.toList();
    if (items.isEmpty()) items = parsed.toMap().value("items").toList();
    for (int i = 0; i < items.size(); ++i) {
        const QVariantMap p = items.at(i).toMap();
        if (p.value("isSelf").toBool()) continue;
        const QString phone = p.value("phoneNumber").toString();
        if (!phone.isEmpty()) return phone;
    }
    return QString();
}
