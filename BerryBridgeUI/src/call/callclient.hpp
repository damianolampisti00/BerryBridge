#ifndef CALLCLIENT_HPP_
#define CALLCLIENT_HPP_

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class QLocalSocket;

// The UI side of WhatsApp calls: a thin client of the headless service's
// CallService (BerryBridgeUIService/src/call/callservice.hpp), which owns the
// call, its audio and the gateway link -- so a call goes on when the UI is
// closed. Talks over the local socket in the app's data folder; QML binds to
// the properties and calls the methods.
class CallClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)      // idle incoming outgoing active ended
    Q_PROPERTY(QString phone READ phone NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    Q_PROPERTY(bool muted READ muted NOTIFY changed)
    Q_PROPERTY(bool speaker READ speaker NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed) // the call has been answered (timer running)
    Q_PROPERTY(QString elapsed READ elapsed NOTIFY tick)     // "m:ss"
    Q_PROPERTY(QString link READ link NOTIFY changed)        // gateway link state, for Settings
public:
    explicit CallClient(QObject *parent = 0);

    QString state() const { return m_status.value("state", "idle").toString(); }
    QString phone() const { return m_status.value("phone").toString(); }
    QString name() const { return m_status.value("name").toString(); }
    QString reason() const { return m_status.value("reason").toString(); }
    bool muted() const { return m_status.value("muted").toBool(); }
    bool speaker() const { return m_status.value("speaker").toBool(); }
    bool connected() const { return m_status.value("since").toLongLong() > 0; }
    QString elapsed() const;
    QString link() const { return m_status.value("link", "service not running").toString(); }

    Q_INVOKABLE void startCall(const QString &phone, const QString &name);
    Q_INVOKABLE void accept();
    Q_INVOKABLE void reject();
    Q_INVOKABLE void hangup();
    Q_INVOKABLE void setMuted(bool on);
    Q_INVOKABLE void setSpeaker(bool on);
    // After Settings saved callServerUrl/callToken.
    Q_INVOKABLE void reloadConfig();
    // The WhatsApp phone number of a one-to-one chat ("+39..."), from the
    // participants Beeper reported; empty if unknown.
    Q_INVOKABLE QString phoneForChat(const QString &chatID) const;
    Q_INVOKABLE bool isWhatsAppAccount(const QString &accountID) const;

signals:
    void changed();
    void tick();
    // A call needs the call screen (it rings, was started, or is going on).
    void callScreenNeeded();

private slots:
    void connectToService();
    void onReadyRead();
    void onDisconnected();

private:
    void send(const QVariantMap &cmd);

    QLocalSocket *m_socket;
    QTimer m_retry;
    QTimer m_ticker;
    QVariantMap m_status;
};

#endif /* CALLCLIENT_HPP_ */
