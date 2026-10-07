#ifndef CALLSERVICE_HPP_
#define CALLSERVICE_HPP_

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSettings>
#include <QVariantMap>

#include "callaudio.hpp"

class CallLink;
class QLocalServer;
class QLocalSocket;
class QThread;
namespace bb {
namespace platform { class Notification; class NotificationDialog; }
namespace system { class InvokeManager; }
}

// WhatsApp calls on the phone (see the call design): the WhatsApp side runs
// on the server (WaCalls + the Berry Bridge gateway, bbgateway.go); this
// headless service keeps ONE persistent link to the gateway (CallLink, its own
// thread), turns gateway events into a call state, rings (NotificationDialog
// + Hub), runs the call audio (CallAudioInput/Output) and serves the UI.
//
// UI <-> service: a local socket in the app's private data folder (only this
// app can reach it), newline-delimited JSON.
//   UI -> service: {"cmd":"status"} {"cmd":"start","phone":"+39..","name":".."}
//                  {"cmd":"accept"} {"cmd":"reject"} {"cmd":"hangup"}
//                  {"cmd":"mute","on":true} {"cmd":"speaker","on":true} {"cmd":"config"}
//   service -> UI: {"event":"state","state":"idle|incoming|outgoing|active|ended",
//                   "callId":..,"phone":..,"name":..,"since":<ms epoch of answer>,
//                   "muted":..,"speaker":..,"reason":..,"link":"open|..."}
// Configuration: QSettings callServerUrl (wss://.../ws) and callToken.
class CallService : public QObject
{
    Q_OBJECT
public:
    explicit CallService(QObject *parent = 0);
    ~CallService();

    static QString socketPath();

public slots:
    // Re-read callServerUrl/callToken and (re)connect.
    void reloadConfig();

private slots:
    void onLinkOpened();
    void onLinkClosed(const QString &reason);
    void onGatewayText(const QByteArray &utf8);
    void onNewClient();
    void onClientData();
    void onClientGone();
    void onRingDialogFinished();
    void finishEnded();

private:
    enum State { Idle, Incoming, Outgoing, Active, Ended };
    void handleCommand(const QVariantMap &cmd);
    void sendGateway(const QVariantMap &msg);
    void setState(State s, const QString &reason = QString());
    void broadcast();
    QVariantMap statusMap() const;
    void startAudio();
    void stopAudio();
    void restartOutput();
    void ring(bool on);
    void postHubNotification(const QString &title, const QString &body, const QString &key);
    void openUi();
    QString nameForPhone(const QString &phone) const;
    void stopLink();

    QSettings m_settings;
    QThread *m_netThread;
    CallLink *m_link;
    QString m_linkState;
    QString m_linkUrl;
    QString m_linkToken;

    QLocalServer *m_server;
    QList<QLocalSocket *> m_clients;

    State m_state;
    QString m_callId;
    QString m_phone;
    QString m_name;
    QString m_reason;
    qint64 m_since;
    bool m_muted;
    bool m_speaker;
    bool m_answered;

    CallAudioConfig m_audioCfg;
    AudioRing *m_upRing;
    AudioRing *m_downRing;
    CallAudioInput *m_input;
    CallAudioOutput *m_output;

    bb::platform::NotificationDialog *m_ringDialog;
    bb::system::InvokeManager *m_invoke;
};

#endif /* CALLSERVICE_HPP_ */
