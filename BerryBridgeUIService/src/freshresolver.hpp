#ifndef FRESHRESOLVER_HPP_
#define FRESHRESOLVER_HPP_

#include <QDebug>
#include <QDir>
#include <QHostAddress>
#include <QSettings>
#include <QThread>

#include <arpa/inet.h>
#include <netdb.h>
#include <resolv.h>
#include <string.h>

// Resolves a host name in a brand-new thread, after reloading the DNS
// configuration, and falls back to the last address found for it.
//
// The service went on getting "Host not found" for hours (no messages,
// no calls) while ping and every other process resolved the same name: after
// some network change, every lookup of this process fails at once, Qt's
// (QHostInfo, pooled threads, failures cached) as well as a fresh thread's
// getaddrinfo() after res_init(). Only a restart of the process cured it.
// The servers we talk to sit behind Cloudflare's anycast addresses, which
// stay the same for months: when the lookup fails, the last address that
// worked (saved across restarts) is used instead, and the failure is logged
// with the resolver's own reason.
//
// Emits resolved() in the thread that created it, then deletes itself; a
// null address means the lookup failed and nothing was known for the name.
class FreshResolver : public QThread
{
    Q_OBJECT
public:
    explicit FreshResolver(const QString &host, QObject *parent = 0) :
            QThread(parent), m_host(host), m_error(0)
    {
        connect(this, SIGNAL(finished()), this, SLOT(report()));
    }

signals:
    void resolved(const QString &host, const QHostAddress &address);

protected:
    void run()
    {
        if (m_address.setAddress(m_host)) return; // already an address
        res_init();
        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = 0;
        m_error = getaddrinfo(m_host.toUtf8().constData(), 0, &hints, &res);
        if (m_error == 0 && res) {
            m_address = QHostAddress(res->ai_addr);
            freeaddrinfo(res);
        }
    }

private slots:
    void report()
    {
        QSettings cache(QDir::homePath() + "/dnscache.ini", QSettings::IniFormat);
        const QString key = "hosts/" + m_host;
        if (!m_address.isNull()) {
            if (cache.value(key).toString() != m_address.toString()) cache.setValue(key, m_address.toString());
        } else {
            const QString last = cache.value(key).toString();
            qWarning() << "[DNS] lookup of" << m_host << "failed:" << m_error << gai_strerror(m_error)
                       << (last.isEmpty() ? "- no address known" : "- using the last one, " + last);
            if (!last.isEmpty()) m_address.setAddress(last);
        }
        emit resolved(m_host, m_address);
        deleteLater();
    }

private:
    QString m_host;
    QHostAddress m_address;
    int m_error;
};

#endif /* FRESHRESOLVER_HPP_ */
