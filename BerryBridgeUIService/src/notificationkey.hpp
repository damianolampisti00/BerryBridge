#ifndef NOTIFICATIONKEY_HPP_
#define NOTIFICATIONKEY_HPP_

#include <QCryptographicHash>
#include <QString>

// The key of a chat's message notification (bb::platform::Notification). Not
// the chatID itself: Beeper chat IDs contain ':' ("!abc:beeper.local") and the
// lock screen's notification card cuts the key there, asks notification_service
// for "!abc", gets REQUEST_KEY_DOES_NOT_EXISTS and shows an error. The Hub
// copes, the lock screen does not. A hex hash has no ':' and stays one-to-one.
// Shared by the service (Service::createMessageNotification) and the app
// (ApplicationUI::dismissChatNotification, which includes it from here).
inline QString bbChatNotificationKey(const QString &chatID)
{
    return "chat-" + QString::fromLatin1(
            QCryptographicHash::hash(chatID.toUtf8(), QCryptographicHash::Md5).toHex());
}

#endif /* NOTIFICATIONKEY_HPP_ */
