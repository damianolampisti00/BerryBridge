#ifndef STICKERANIMATOR_HPP_
#define STICKERANIMATOR_HPP_

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <bb/cascades/ImageView>

struct WebPAnimDecoder;

// Plays an animated sticker in a chat bubble's ImageView. Cascades can't
// animate any image format by itself, so this decodes the sticker's WebP one
// frame at a time (libwebp, see webpimage.hpp) and hands each frame to the
// ImageView, at the sticker's own timing. Only the compressed file and one
// frame are in memory at a time.
//
// To spare the battery it plays kLoops times, then rests on the first frame
// (play() starts it again, e.g. on a tap), and pauses while the app is
// minimized or hidden.
//
// In QML, as an attached object of the ImageView:
//     StickerAnimator { imageView: stickerImage; source: ListItemData.localImagePath }
// source is the sticker's cached PNG (its first frame, see Database::stickerFor);
// the WebP next to it (animationPathFor) exists only for an animated sticker.
// Without it the PNG is shown as it is.
class StickerAnimator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bb::cascades::ImageView *imageView READ imageView WRITE setImageView NOTIFY imageViewChanged)
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool animated READ animated NOTIFY animatedChanged)
public:
    explicit StickerAnimator(QObject *parent = 0);
    virtual ~StickerAnimator();

    bb::cascades::ImageView *imageView() const { return m_view; }
    void setImageView(bb::cascades::ImageView *view);
    QString source() const { return m_source; }
    void setSource(const QString &source);
    bool animated() const { return m_decoder != 0; }

    // ".../x.png" -> ".../x.webp"
    static QString animationPathFor(const QString &pngPath);

    Q_INVOKABLE void play();

Q_SIGNALS:
    void imageViewChanged();
    void sourceChanged();
    void animatedChanged();

private Q_SLOTS:
    void nextFrame();
    void onAppHidden();
    void onAppShown();

private:
    void load();
    void unload();
    void showFirstFrame();
    bool showNextFrame(int *timestamp);

    QPointer<bb::cascades::ImageView> m_view;
    QString m_source;
    QByteArray m_data; // the WebP; the decoder reads from it
    WebPAnimDecoder *m_decoder;
    int m_width, m_height;
    int m_lastTimestamp;
    int m_loopsLeft;
    bool m_appVisible;
    QTimer m_timer;
};

#endif /* STICKERANIMATOR_HPP_ */
