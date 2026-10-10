#include "viewonceclient.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegExp>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <bb/ImageData>
#include <bb/cascades/Image>
#include <bb/data/JsonDataAccess>
#include <bb/utility/ImageConverter>

namespace {
const char *kVideoPrefix = "viewonce_";
const int kVideoLifetimeMs = 60 * 1000;
}

ViewOnceClient::ViewOnceClient(QObject *parent) :
        QObject(parent), m_nam(new QNetworkAccessManager(this)), m_reply(0), m_busy(false)
{
    // A video left behind by a crash or a kill: never keep one.
    QDir tmp(QDir::tempPath());
    foreach (const QString &f, tmp.entryList(QStringList() << QString(kVideoPrefix) + "*", QDir::Files)) {
        tmp.remove(f);
    }
}

bool ViewOnceClient::configured() const
{
    QSettings s;
    return !s.value("viewOnceUrl").toString().isEmpty() && !s.value("viewOnceToken").toString().isEmpty();
}

bool ViewOnceClient::isViewOnceNotice(const QString &text)
{
    // mautrix-instagram: "This %s can only be %s once. Use the Instagram app to view."
    return QRegExp("can only be \\w+ once\\. Use the Instagram app").indexIn(text) >= 0;
}

void ViewOnceClient::open(const QString &utcTimestamp)
{
    if (m_reply) return;
    QSettings s;
    const QString base = s.value("viewOnceUrl").toString().trimmed();
    const QString token = s.value("viewOnceToken").toString().trimmed();
    if (base.isEmpty() || token.isEmpty()) {
        emit failed(tr("Server per le foto a visualizzazione singola non configurato"));
        return;
    }
    QDateTime when = QDateTime::fromString(utcTimestamp, Qt::ISODate);
    if (!when.isValid()) when = QDateTime::fromString(utcTimestamp.left(19), "yyyy-MM-ddTHH:mm:ss");
    when.setTimeSpec(Qt::UTC);
    if (!when.isValid()) {
        emit failed(tr("Orario del messaggio non leggibile"));
        return;
    }
    QUrl url(base + "/v1/open");
    url.addQueryItem("ts", QString::number(when.toTime_t()));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    m_reply = m_nam->get(req);
    connect(m_reply, SIGNAL(finished()), this, SLOT(onFinished()));
    setBusy(true);
}

void ViewOnceClient::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = 0;
    setBusy(false);
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();

    if (status != 200) {
        QString msg = bb::data::JsonDataAccess().loadFromBuffer(body).toMap().value("message").toString();
        if (msg.isEmpty()) {
            if (status == 404) msg = tr("Già vista, scaduta o non trovata");
            else if (status == 401) msg = tr("Token del server non valido");
            else if (status == 0) msg = tr("Server non raggiungibile");
            else msg = tr("Errore del server (%1)").arg(status);
        }
        emit failed(msg);
        return;
    }
    if (type.startsWith("video/")) {
        const QString path = QDir::tempPath() + "/" + kVideoPrefix
                + QUuid::createUuid().toString().mid(1, 8) + ".mp4";
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(body) != body.size()) {
            emit failed(tr("Impossibile preparare il video"));
            return;
        }
        f.close();
        m_videos << path;
        // The player keeps the file open: removing it a minute later is safe.
        QTimer::singleShot(kVideoLifetimeMs, this, SLOT(deleteOldVideos()));
        emit videoReady(path);
        return;
    }
    const bb::ImageData data = bb::utility::ImageConverter::decode(body);
    if (!data.isValid()) {
        emit failed(tr("Immagine non leggibile"));
        return;
    }
    m_image = QVariant::fromValue(bb::cascades::Image(data));
    emit imageChanged();
    emit imageReady();
}

void ViewOnceClient::clear()
{
    m_image = QVariant();
    emit imageChanged();
}

void ViewOnceClient::deleteOldVideos()
{
    foreach (const QString &p, m_videos) QFile::remove(p);
    m_videos.clear();
}

void ViewOnceClient::setBusy(bool b)
{
    if (b == m_busy) return;
    m_busy = b;
    emit busyChanged();
}
