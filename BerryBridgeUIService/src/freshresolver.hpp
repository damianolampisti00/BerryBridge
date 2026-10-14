#ifndef FRESHRESOLVER_HPP_
#define FRESHRESOLVER_HPP_

#include <QHostAddress>
#include <QThread>

#include <arpa/inet.h>
#include <netdb.h>
#include <resolv.h>
#include <string.h>

// Resolves a host name in a brand-new thread, after reloading the DNS
// configuration. Qt's own lookups (QHostInfo) run in pooled threads that stay
// alive while the service keeps retrying, with the resolver state they read
// once, and Qt caches failed lookups too: after a network hiccup the service
// went on getting "Host not found" for good (seen: 16 min, push link down,
// no messages) while every other process resolved the name fine. res_init()
// in our own thread never reached those workers. A fresh thread starts from
// a fresh state.
//
// Emits resolved() in the thread that created it, then deletes itself; a
// null address means the lookup failed.
class FreshResolver : public QThread
{
    Q_OBJECT
public:
    explicit FreshResolver(const QString &host, QObject *parent = 0) :
            QThread(parent), m_host(host)
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
        if (getaddrinfo(m_host.toUtf8().constData(), 0, &hints, &res) == 0 && res) {
            m_address = QHostAddress(res->ai_addr);
            freeaddrinfo(res);
        }
    }

private slots:
    void report()
    {
        emit resolved(m_host, m_address);
        deleteLater();
    }

private:
    QString m_host;
    QHostAddress m_address;
};

#endif /* FRESHRESOLVER_HPP_ */
