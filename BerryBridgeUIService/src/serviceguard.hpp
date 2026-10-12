#ifndef SERVICEGUARD_HPP_
#define SERVICEGUARD_HPP_

#include <QAbstractSocket>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>

// Closes a socket whose notifier keeps throwing out of its event handler
// (std::bad_alloc from Qt reading a broken connection: the notifier fires
// again at once, the event loop spins and nothing else gets done). Posted to
// the socket's thread, after the failing event; the socket's owner sees an
// ordinary disconnection and reconnects (push link, call gateway) or drops it
// (the UI's call-control connection).
class SocketCloser : public QObject
{
    Q_OBJECT
public:
    explicit SocketCloser(QObject *socket) : m_socket(socket) {}

public slots:
    void run()
    {
        if (QAbstractSocket *s = qobject_cast<QAbstractSocket *>(m_socket)) s->abort();
        else if (QLocalSocket *s = qobject_cast<QLocalSocket *>(m_socket)) s->abort();
        deleteLater();
    }

private:
    QPointer<QObject> m_socket;
};

#endif /* SERVICEGUARD_HPP_ */
