#include "callringer.hpp"

#include <QDateTime>
#include <QDebug>
#include <QVariantList>
#include <QVariantMap>
#include <bb/data/JsonDataAccess>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

namespace {
const char *const kNotifyControl = "/pps/services/notify/control";

QVariantMap choice(const QString &context, const QString &label)
{
    QVariantMap c;
    c["context"] = context;
    c["label"] = label;
    c["lockBehaviour"] = "show";
    return c;
}

bool writeAll(int fd, const QByteArray &msg)
{
    return ::write(fd, msg.constData(), msg.size()) == msg.size();
}
}

CallRinger::CallRinger(QObject *parent) :
        QObject(parent),
        m_fd(-1),
        m_invokeFd(-1),
        m_wakeFd(-1)
{
    m_poll.setInterval(150);
    connect(&m_poll, SIGNAL(timeout()), this, SLOT(poll()));
}

CallRinger::~CallRinger()
{
    stop();
    closeInvoke();
    closeWake();
}

bool CallRinger::ring(const QString &title, const QString &body)
{
    stop();
    m_fd = ::open(kNotifyControl, O_RDWR | O_NONBLOCK);
    if (m_fd < 0) {
        qWarning() << "[CALL] ringer: open" << kNotifyControl << strerror(errno);
        return false;
    }
    const QString id = "call-dialog-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    QVariantMap dat;
    dat["itemId"] = id;
    dat["title"] = title;
    dat["subtitle"] = body;
    dat["repeating"] = "on";             // rings until answered
    QVariantList choices;
    choices << choice("answer", "Rispondi") << choice("reject", "Rifiuta");
    dat["promptChoices"] = choices;

    QString json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(dat, &json);
    json.replace('\n', ' '); // one PPS attribute per line
    const QByteArray msg = "msg::alert\nid::" + id.toUtf8() + "\ndat:json:" + json.toUtf8() + "\n";
    if (!writeAll(m_fd, msg)) {
        qWarning() << "[CALL] ringer: write failed" << strerror(errno);
        stop();
        return false;
    }
    m_pending.clear();
    m_poll.start();
    return true;
}

void CallRinger::stop()
{
    m_poll.stop();
    if (m_fd >= 0) {
        ::close(m_fd); // the notification service cancels the dialog
        m_fd = -1;
    }
}

void CallRinger::poll()
{
    if (m_fd < 0) return;
    char buf[2048];
    ssize_t n;
    while ((n = ::read(m_fd, buf, sizeof buf)) > 0) m_pending.append(buf, n);
    // Replies come as "@control\nid::...\nres::alert\ndat::ok" and, on a
    // button press, "...\nmsg::choice\ndat:json:{"context":"answer",...}".
    if (!m_pending.contains("msg::choice")) {
        if (m_pending.size() > 16384) m_pending.clear();
        return;
    }
    const int at = m_pending.indexOf("dat:json:", m_pending.indexOf("msg::choice"));
    const int start = (at >= 0) ? at : m_pending.indexOf("dat:json:");
    QString context;
    if (start >= 0) {
        int end = m_pending.indexOf('\n', start);
        if (end < 0) end = m_pending.size();
        bb::data::JsonDataAccess jda;
        context = jda.loadFromBuffer(m_pending.mid(start + 9, end - start - 9)).toMap().value("context").toString();
    }
    m_pending.clear();
    stop();
    qDebug() << "[CALL] ringer: chosen" << context;
    emit chosen(context);
}

void CallRinger::invokeItem(const QString &itemId)
{
    closeInvoke();
    m_invokeFd = ::open(kNotifyControl, O_RDWR | O_NONBLOCK);
    if (m_invokeFd < 0) {
        qWarning() << "[CALL] invokeItem: open" << strerror(errno);
        return;
    }
    const QByteArray msg = "msg::invokeItem\nid::invoke\ndat:json:{\"itemId\":\"" + itemId.toUtf8() + "\"}\n";
    if (!writeAll(m_invokeFd, msg)) qWarning() << "[CALL] invokeItem: write failed" << strerror(errno);
    // Kept open a moment: the service handles the request asynchronously.
    QTimer::singleShot(3000, this, SLOT(closeInvoke()));
}

void CallRinger::wakeScreen(const QString &reason)
{
    closeWake();
    m_wakeFd = ::open("/pps/services/navigator/control", O_RDWR | O_NONBLOCK);
    if (m_wakeFd < 0) {
        qWarning() << "[CALL] wake: open" << strerror(errno);
        return;
    }
    QString r = reason;
    r.replace('\n', ' ');
    const QByteArray msg = "msg::requestDeviceUnlock\nid::wake\ndat::" + r.toUtf8() + "\n";
    if (!writeAll(m_wakeFd, msg)) qWarning() << "[CALL] wake: write failed" << strerror(errno);
    QTimer::singleShot(3000, this, SLOT(closeWake()));
}

void CallRinger::closeWake()
{
    if (m_wakeFd >= 0) {
        ::close(m_wakeFd);
        m_wakeFd = -1;
    }
}

void CallRinger::closeInvoke()
{
    if (m_invokeFd >= 0) {
        ::close(m_invokeFd);
        m_invokeFd = -1;
    }
}
