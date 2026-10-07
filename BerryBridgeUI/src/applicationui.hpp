/*
 * Copyright (c) 2013-2015 BlackBerry Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ApplicationUI_HPP_
#define ApplicationUI_HPP_

#include <QObject>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QVariant>
#include <QNetworkAccessManager>

namespace bb {
    namespace cascades {
        class LocaleHandler;
    }
    namespace system {
        class InvokeManager;
        class InvokeRequest;
    }
}

class QTranslator;

/*!
 * @brief Application UI object
 *
 * Use this object to create and init app UI, to create context objects, to register the new meta types etc.
 */
class ApplicationUI: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int dbUpdateTrigger READ dbUpdateTrigger NOTIFY dbUpdateTriggerChanged)
    // Settings > Dark theme (saved; on by default). Switches the Cascades
    // visual style live, and `colors` -- the app's own palette, by role
    // (chatBg, panel, incoming, quote, text, muted, sender, selection, audio,
    // outgoing, avatarMask) -- with it. The input bar stays dark in both,
    // as in the original.
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY darkThemeChanged)

public:
    ApplicationUI();
    virtual ~ApplicationUI() { }

    // servis veritabanını güncelleyince olması gereken yenileme
    int dbUpdateTrigger() const { return m_dbUpdateTrigger; }
    Q_INVOKABLE void setActionBarColor(QString hexColor);
    Q_INVOKABLE QString getFileSizeFormatted(const QString &filePath);
    Q_INVOKABLE QVariant getSetting(const QString &key, const QVariant &defaultValue);
    Q_INVOKABLE void updateSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void clearNewContent(const QString &accountId);
    Q_INVOKABLE bool hasNewContent(const QString &accountId) const;
    // main.qml, once its tabs exist: from now on openChatRequested is emitted
    // directly, and a chat requested before that (the notification launched
    // the app) is emitted now.
    Q_INVOKABLE void takePendingChat();
    // A chat was opened: its notification leaves the Hub.
    Q_INVOKABLE void dismissChatNotification(const QString &chatID);

    bool darkTheme() const;
    void setDarkTheme(bool dark);
    QVariantMap colors() const;

signals:
    void darkThemeChanged();
    // servis veritabanını güncelleyince olması gereken yenileme
    // main.qml burayı dinlemede
    void dbUpdateTriggerChanged();
    void updateCheckCompleted(bool updateRequired, QString latestVersion, QString releaseNotes);
    void updateCheckFailed(QString errorMessage);
    // A tapped message notification: show this chat.
    void openChatRequested(const QString &accountID, const QString &chatID);

private slots:
    void onSystemLanguageChanged();
    void onUIRefreshTriggered(const QString &path);
    void onReplyFinished(QNetworkReply* reply);
    void onInvoked(const bb::system::InvokeRequest &request);
private:
    QTranslator* m_translator;
    bb::cascades::LocaleHandler* m_localeHandler;
    bb::system::InvokeManager* m_invokeManager;
    QFileSystemWatcher* m_uiWatcher;
    int m_dbUpdateTrigger;
    QSettings m_settings;
    QNetworkAccessManager* m_networkManager;
    bool m_qmlReadyForChats;
    QString m_pendingChatAccount;
    QString m_pendingChatID;
    void applyVisualStyle();
    bool isVersionGreater(const QString& current, const QString& latest);
    QString formatMarkdownToHtml(const QString& markdown);
};

#endif /* ApplicationUI_HPP_ */
