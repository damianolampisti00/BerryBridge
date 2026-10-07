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

#include "applicationui.hpp"
#include "Database.hpp"
#include "ScreenManager.hpp"
#include "audio/voicerecorder.hpp"
#include "audio/voiceplayer.hpp"

#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <bb/cascades/LocaleHandler>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>
#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QDir>
#include <QDateTime>
#include <QtDeclarative/QDeclarativeContext>
#include <bb/cascades/ThemeSupport>
#include <bb/cascades/Theme>
#include <bb/cascades/ColorTheme>
#include <bb/cascades/VisualStyle>
#include <bb/platform/Notification>
#include <bb/platform/NotificationDefaultApplicationSettings>
#include <bb/platform/NotificationPriorityPolicy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <bb/data/JsonDataAccess>
#include <bb/ApplicationInfo>
#include <bb/device/HardwareInfo>

using namespace bb::cascades;
using namespace bb::system;

ApplicationUI::ApplicationUI() :
        QObject(),
        m_translator(new QTranslator(this)),
        m_localeHandler(new LocaleHandler(this)),
        m_invokeManager(new InvokeManager(this)),
        m_uiWatcher(new QFileSystemWatcher(this)),
        m_dbUpdateTrigger(0)
{
    // prepare the localization
    if (!QObject::connect(m_localeHandler, SIGNAL(systemLanguageChanged()),
            this, SLOT(onSystemLanguageChanged()))) {
        // This is an abnormal situation! Something went wrong!
        // Add own code to recover here
        qWarning() << "Recovering from a failed connect()";
    }

    // Connected before the event loop runs, so the invocation that cold-starts
    // the app (a tapped notification) is delivered here too.
    m_qmlReadyForChats = false;
    connect(m_invokeManager, SIGNAL(invoked(const bb::system::InvokeRequest&)),
            this, SLOT(onInvoked(const bb::system::InvokeRequest&)));

    // Opt this app's notifications into Instant Preview (see the same call in
    // the service's constructor): whichever process starts first wins, the
    // other call is a no-op.
    bb::platform::NotificationDefaultApplicationSettings notifySettings;
    notifySettings.setPreview(bb::platform::NotificationPriorityPolicy::Allow);
    notifySettings.apply();

    // Ensure directory and generic refresh file exist before watching
    QDir dir("/accounts/1000/shared/misc/BerryBridge");
    if (!dir.exists()) {
       dir.mkpath(".");
    }
    QString refreshPath = "/accounts/1000/shared/misc/BerryBridge/ui_refresh_trigger.txt";
    QFile refreshFile(refreshPath);
    if (!refreshFile.exists()) {
       if (refreshFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
           refreshFile.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
           refreshFile.close();
       }
    }

    // Set up QFileSystemWatcher for cross-process IPC refresh
    m_uiWatcher->addPath(refreshPath);
    if (!QObject::connect(m_uiWatcher, SIGNAL(fileChanged(const QString &)),
           this, SLOT(onUIRefreshTriggered(const QString &)))) {
       qWarning() << "Failed to connect file watcher";
    }

    m_networkManager = new QNetworkAccessManager(this);

    // Slot imzasına (QNetworkReply*) eklendi
    connect(m_networkManager, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(onReplyFinished(QNetworkReply*)));

    // initial load
    onSystemLanguageChanged();

    // Before the QML exists, so it never shows up in the other style first.
    applyVisualStyle();

    // Create scene document from main.qml asset, the parent is set
    // to ensure the document gets destroyed properly at shut down.
    QmlDocument *qml = QmlDocument::create("asset:///main.qml").parent(this);

    // Make app available to the qml.
    qml->setContextProperty("app", this);
    Database *dat = new Database(this);
    qml->setContextProperty("dat", dat);
    ScreenManager *screenManager = new ScreenManager(this);
    qml->setContextProperty("screenManager", screenManager);
    // Voice messages: recording (PCM -> Ogg/Opus, no BerryCore) and in-chat playback.
    qml->setContextProperty("voiceRecorder", new VoiceRecorder(this));
    qml->setContextProperty("voicePlayer", new VoicePlayer(this));

    // Create root object for the UI
    AbstractPane *root = qml->createRootObject<AbstractPane>();

    // Set created root object as the application scene
    Application::instance()->setScene(root);

    // Make sure the headless service runs: after an install (or a crash) the
    // system only starts it at the next boot. Invoking it starts it; RESET is
    // a declared action it otherwise ignores.
    bb::system::InvokeRequest startService;
    startService.setTarget("it.berrybridge.service");
    startService.setAction("it.berrybridge.service.RESET");
    m_invokeManager->invoke(startService);

    // Opening the app only turns off the LED / splat; each chat's message
    // stays in the Hub until that chat is opened (dismissChatNotification).
    bb::platform::Notification::clearEffectsForAll();
}

void ApplicationUI::onSystemLanguageChanged()
{
    QCoreApplication::instance()->removeTranslator(m_translator);
    // Initiate, load and install the application translation files.
    QString locale_string = QLocale().name();
    QString file_name = QString("BerryBridgeUI_%1").arg(locale_string);
    if (m_translator->load(file_name, "app/native/qm")) {
    QCoreApplication::instance()->installTranslator(m_translator);
    }
}

void ApplicationUI::onUIRefreshTriggered(const QString &path)
{
    //qDebug() << "[APP UI] Refresh file modified via IPC cross-process:" << path;

    // Qt's file watcher can sometimes drop the watch if the file was deleted/replaced
    if (!m_uiWatcher->files().contains(path)) {
        qDebug() << "[APP UI] Path dropped from watcher, re-adding:" << path;
        // servis veritabanını güncelleyince olması gereken yenileme
        m_uiWatcher->addPath(path);
    }

    // No Hub clean-up here: the service rewrites this file for EVERY incoming
    // message, so a running UI (even minimized) used to wipe the Hub right
    // after each notification -- the phone vibrated, the Hub stayed empty.

    m_dbUpdateTrigger++;
    // main.qml burayı dinlemede
    emit dbUpdateTriggerChanged();
}

void ApplicationUI::setActionBarColor(QString hexColor) {
    bool ok;

    // 1. Başındaki '#' işaretini temizle
    if (hexColor.startsWith("#")) {
        hexColor.remove(0, 1);
    }

    if (hexColor.length() == 6) {
        hexColor.prepend("ff");
    }

    // 3. String'i unsigned int (hexadecimal) formatına çevir
    unsigned int rgba = hexColor.toUInt(&ok, 16);

    if (ok) {
        // ARTIK STATİK DEĞİL: Hesaplanan rgba değerini kullanıyoruz
        bb::cascades::Color newColor = bb::cascades::Color::fromARGB(rgba);
        // Temayı güncelle
        bb::cascades::Application::instance()->themeSupport()->setPrimaryColor(newColor);

    }
}

QString ApplicationUI::getFileSizeFormatted(const QString &filePath) {
    // QML'den gelen "file://" önekini temizle
    QString cleanPath = filePath.startsWith("file://") ? QUrl(filePath).toLocalFile() : filePath;

    QFileInfo info(cleanPath);
    if (!info.exists()) {
        return "0 Bytes";
    }

    double bytes = static_cast<double>(info.size());

    if (bytes < 1024.0) {
        return QString::number(bytes, 'f', 0) + " Bytes";
    } else if (bytes < (1024.0 * 1024.0)) {
        return QString::number(bytes / 1024.0, 'f', 0) + " KB";
    } else if (bytes < (1024.0 * 1024.0 * 1024.0)) {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    } else {
        return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
    }
}

bool ApplicationUI::darkTheme() const
{
    return m_settings.value("darkTheme", true).toBool();
}

void ApplicationUI::setDarkTheme(bool dark)
{
    if (dark == darkTheme()) return;
    m_settings.setValue("darkTheme", dark);
    m_settings.sync();
    applyVisualStyle();
    emit darkThemeChanged();
}

void ApplicationUI::applyVisualStyle()
{
    Application::instance()->themeSupport()->setVisualStyle(
            darkTheme() ? VisualStyle::Dark : VisualStyle::Bright);
}

// The app's own colors, by role. Dark: WhatsApp's dark palette; light: the
// colors of the original Berry Bridge. Per-network chat colors are in
// GenericTab.qml's getDefaultColors.
QVariantMap ApplicationUI::colors() const
{
    QVariantMap c;
    if (darkTheme()) {
        c["chatBg"] = "#0B141A";
        c["panel"] = "#1F2C33";
        c["incoming"] = "#202C33";
        c["quote"] = "#2A3942";
        c["text"] = "#FFFFFF";
        c["muted"] = "#8696A0";
        c["sender"] = "#AEBAC1";
        c["selection"] = "#3B4A54";
        c["audio"] = "#53BDEB";
        c["outgoing"] = "#005C4B";
        c["avatarMask"] = "asset:///images/bPro.png";
    } else {
        c["chatBg"] = "#E5DDD5";
        c["panel"] = "#F5F5F5";
        c["incoming"] = "#FFFFFF";
        c["quote"] = "#F6F5F3";
        c["text"] = "#000000";
        c["muted"] = "#666666";
        c["sender"] = "#444444";
        c["selection"] = "#C0C0C0";
        c["audio"] = "#00008B";
        c["outgoing"] = "#D8FDD2";
        c["avatarMask"] = "asset:///images/wPro.png";
    }
    return c;
}

void ApplicationUI::dismissChatNotification(const QString &chatID)
{
    // The service keys each chat's notification by its chatID (Service::createMessageNotification).
    bb::platform::Notification::deleteFromInbox(chatID);
}

QVariant ApplicationUI::getSetting(const QString &key, const QVariant &defaultValue) {
    return m_settings.value(key, defaultValue);
}

void ApplicationUI::updateSetting(const QString &key, const QVariant &value) {
    m_settings.setValue(key, value);
    m_settings.sync();
}

void ApplicationUI::checkForUpdates() {
    QNetworkRequest request;

    // Cihaz PIN'ini ve Uygulama Sürümünü Al
    bb::device::HardwareInfo hwInfo;
    QString devicePin = hwInfo.pin();

    bb::ApplicationInfo appInfo;
    QString appVersion = appInfo.version();

    // URL'yi oluştur ve query parametrelerini ekle
    QUrl url("https://update-check-beeper.zead29.workers.dev/");
    url.addQueryItem("device_id", devicePin);
    url.addQueryItem("app_version", appVersion);

    request.setUrl(url);
    request.setRawHeader("User-Agent", "Mozilla/5.0 (BB10; BlackBerry)");

    m_networkManager->get(request);
}

void ApplicationUI::onReplyFinished(QNetworkReply* reply) {
    if (!reply) return;

    // 1. Ağ / Bağlantı seviyesi hatası (İnternet yok, SSL hatası, zaman aşımı vb.)
    if (reply->error() != QNetworkReply::NoError) {
        QString errorStr = reply->errorString();
        emit updateCheckFailed("Network Error!");
        reply->deleteLater();
        return;
    }

    // 2. HTTP Yanıt Kodu Kontrolü (200 OK dışındaki 403, 404, 500 gibi durumlar)
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus != 200) {
        emit updateCheckFailed("Server Error!");
        reply->deleteLater();
        return;
    }

    QByteArray response = reply->readAll();

    // 3. JSON Ayrıştırma Kontrolü
    bb::data::JsonDataAccess jda;
    QVariant jsonResult = jda.loadFromBuffer(response);

    if (jda.hasError()) {
        emit updateCheckFailed("Data Error!");
        reply->deleteLater();
        return;
    }

    QVariantMap resultMap = jsonResult.toMap();

    // Worker içinden "error" anahtarı döndüyse
    if (resultMap.contains("error")) {
        emit updateCheckFailed("API Error!");
        reply->deleteLater();
        return;
    }

    QString tagName = resultMap["tag_name"].toString();
    if (tagName.isEmpty()) {
        emit updateCheckFailed("Version info not found!");
        reply->deleteLater();
        return;
    }

    // YENİ EKLENEN: Sürüm notlarını değişkene al
    QString rawNotes = resultMap["release_notes"].toString();

    // 4. Versiyon Karşılaştırma
    QString latestVersion = tagName.startsWith("v", Qt::CaseInsensitive)
                            ? tagName.mid(1) : tagName;

    bb::ApplicationInfo appInfo;
    QString currentVersion = appInfo.version();

    bool updateRequired = isVersionGreater(currentVersion, latestVersion);
    // Markdown'ı QML uyumlu HTML'e çevir
    QString formattedReleaseNotes = formatMarkdownToHtml(rawNotes);

    // Sinyal ile arayüze aktar
    emit updateCheckCompleted(updateRequired, tagName, formattedReleaseNotes);

    reply->deleteLater();
}

// Versiyon kıyaslama algoritması (Örn: 1.0.0.1 < 1.0.0.2)
bool ApplicationUI::isVersionGreater(const QString& current, const QString& latest) {
    QStringList currParts = current.split('.');
    QStringList latParts = latest.split('.');

    int maxParts = qMax(currParts.size(), latParts.size());

    for (int i = 0; i < maxParts; ++i) {
        int currVal = (i < currParts.size()) ? currParts[i].toInt() : 0;
        int latVal = (i < latParts.size()) ? latParts[i].toInt() : 0;

        if (latVal > currVal) return true;
        if (latVal < currVal) return false;
    }

    return false; // Aynı sürüm veya mevcut sürüm daha yüksek
}

#include <QString>
#include <QStringList>
#include <QRegExp>

QString ApplicationUI::formatMarkdownToHtml(const QString& markdown) {
    QString text = markdown;

    // 1. Windows satır sonlarını (\r\n) Unix formatına (\n) çevir
    text.replace("\r\n", "\n");

    // 2. Markdown Link Dönüştürme: [Title](URL) -> <a href="URL">Title</a>
    QRegExp linkRx("\\[([^\\]]+)\\]\\(([^)]+)\\)");
    text.replace(linkRx, "<a href=\"\\2\">\\1</a>");

    // 3. Kalın Metin Dönüştürme: **text** -> <b>text</b>
    QRegExp boldRx("\\*\\*(.*?)\\*\\*");
    boldRx.setMinimal(true); // Açgözlü (greedy) eşleşmeyi önler
    text.replace(boldRx, "<b>\\1</b>");

    // 4. Satır satır Liste ve Başlık İşleme
    QStringList lines = text.split("\n");
    QStringList formattedLines;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();

        // Liste maddeleri (- veya *) -> Maddeli Simge (&bull;)
        if (line.startsWith("- ") || line.startsWith("* ")) {
            line = "&bull; " + line.mid(2);
        }
        // Başlıklar (# Header) -> Kalın Başlık
        else if (line.startsWith("#")) {
            int hashCount = 0;
            while (hashCount < line.length() && line.at(hashCount) == '#') {
                hashCount++;
            }
            QString headerText = line.mid(hashCount).trimmed();
            line = QString("<b>%1</b>").arg(headerText);
        }

        formattedLines.append(line);
    }

    // 5. Satırları HTML <br/> ile birleştir
    return formattedLines.join("<br/>");
}

void ApplicationUI::onInvoked(const bb::system::InvokeRequest &request)
{
    if (request.mimeType() != "application/x-berrybridge-chat") return;
    const QStringList parts = QString::fromUtf8(request.data()).split('\n');
    if (parts.size() < 2 || parts.at(1).isEmpty()) return;
    if (m_qmlReadyForChats) {
        emit openChatRequested(parts.at(0), parts.at(1));
    } else {
        m_pendingChatAccount = parts.at(0);
        m_pendingChatID = parts.at(1);
    }
}

void ApplicationUI::takePendingChat()
{
    m_qmlReadyForChats = true;
    if (m_pendingChatID.isEmpty()) return;
    const QString account = m_pendingChatAccount, chat = m_pendingChatID;
    m_pendingChatAccount.clear();
    m_pendingChatID.clear();
    emit openChatRequested(account, chat);
}

void ApplicationUI::clearNewContent(const QString &accountId)
{
    QSettings settings;
    // QSettings anahtarı: örn. "notifications/whatsapp" -> true/false
    settings.setValue(QString("newContent/%1").arg(accountId), false);
}

bool ApplicationUI::hasNewContent(const QString &accountId) const
{
    QSettings settings;
    return settings.value(QString("newContent/%1").arg(accountId), false).toBool();
}


