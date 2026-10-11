#ifndef CALLRINGER_HPP_
#define CALLRINGER_HPP_

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

// The incoming-call dialog, written straight to the notification service's
// PPS server (/pps/services/notify/control), as bb::platform::NotificationDialog
// does -- plus what its API doesn't expose:
//  - lockBehaviour "show" on the buttons, so Rispondi/Rifiuta work on the lock
//    screen without unlocking (the library always sends "hide"; the service
//    knows hide, show and unlock),
// Sound and vibration follow the active profile and Berry Bridge's
// notification settings (levelOnePriority would ring also in Silent).
// Closing the connection cancels the dialog (the service then cancels it by
// itself), so stop() just closes it.
//
// invokeItem(): the notification service invokes a Hub notification's target
// for us. A headless process may only invoke cards and services (the navigator
// answers INVOKE_NO_TARGET_ERROR for the UI app), so this is how the call
// service brings up the call screen.
class CallRinger : public QObject
{
    Q_OBJECT
public:
    explicit CallRinger(QObject *parent = 0);
    ~CallRinger();

    bool ring(const QString &title, const QString &body);
    void stop();
    bool isRinging() const { return m_fd >= 0; }

    void invokeItem(const QString &itemId);
    // Turns the screen on, as for a phone call: the navigator's unlock
    // request (what swiping up does). Without a password the phone
    // unlocks; with one, the password prompt shows "reason".
    void wakeScreen(const QString &reason);

signals:
    // A button was pressed: "answer" or "reject".
    void chosen(const QString &context);

private slots:
    void poll();
    void closeInvoke();
    void closeWake();

private:
    int m_fd;
    int m_invokeFd;
    int m_wakeFd;
    QTimer m_poll;
    QByteArray m_pending;
};

#endif /* CALLRINGER_HPP_ */
