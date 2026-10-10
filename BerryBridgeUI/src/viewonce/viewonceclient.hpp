#ifndef VIEWONCECLIENT_HPP_
#define VIEWONCECLIENT_HPP_

#include <QObject>
#include <QStringList>
#include <QVariant>

class QNetworkAccessManager;
class QNetworkReply;

// Instagram view-once photos and videos (QML context property "viewOnce").
// Beeper's bridge only gets a notice for them ("This image can only be viewed
// once. Use the Instagram app to view."); the igview service on the server
// (BerryBridge/igview) fetches the media through Instagram's mobile API.
// open() asks it for the item next to the notice's time: the service streams
// it once and marks it seen on Instagram.
//
// Nothing is kept: a photo lives only in memory (the `image` property) until
// clear(); a video needs a file for the player, which is deleted a minute
// after the player opened it (and any leftover at the next start).
//
// Settings (Berry Bridge's QSettings): viewOnceUrl (e.g.
// https://igview.example.com), viewOnceToken.
class ViewOnceClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariant image READ image NOTIFY imageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool configured READ configured CONSTANT)

public:
    explicit ViewOnceClient(QObject *parent = 0);

    QVariant image() const { return m_image; }
    bool busy() const { return m_busy; }
    bool configured() const;

    // True for the bridge's notice that stands in for a view-once media.
    Q_INVOKABLE static bool isViewOnceNotice(const QString &text);
    // utcTimestamp: the notice message's timestamp as stored (ISO 8601, UTC).
    Q_INVOKABLE void open(const QString &utcTimestamp);
    // Drops the photo from memory (the viewer closed).
    Q_INVOKABLE void clear();

signals:
    void imageChanged();
    void busyChanged();
    void imageReady();
    void videoReady(const QString &path);
    void failed(const QString &message);

private slots:
    void onFinished();
    void deleteOldVideos();

private:
    void setBusy(bool b);

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_reply;
    QVariant m_image;
    bool m_busy;
    QStringList m_videos;
};

#endif /* VIEWONCECLIENT_HPP_ */
