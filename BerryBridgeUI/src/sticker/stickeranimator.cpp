#include "stickeranimator.hpp"

#include <QFile>
#include <QUrl>
#include <bb/ImageData>
#include <bb/cascades/Application>
#include <bb/cascades/Image>
#include <webp/demux.h>
#include <string.h>

static const int kLoops = 3;
static const int kMinFrameMs = 40; // at most 25 frames per second

StickerAnimator::StickerAnimator(QObject *parent)
    : QObject(parent)
    , m_decoder(0)
    , m_width(0)
    , m_height(0)
    , m_lastTimestamp(0)
    , m_loopsLeft(0)
    , m_appVisible(true)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(nextFrame()));
    bb::cascades::Application *app = bb::cascades::Application::instance();
    if (app) {
        connect(app, SIGNAL(thumbnail()), this, SLOT(onAppHidden()));
        connect(app, SIGNAL(invisible()), this, SLOT(onAppHidden()));
        connect(app, SIGNAL(fullscreen()), this, SLOT(onAppShown()));
    }
}

StickerAnimator::~StickerAnimator()
{
    unload();
}

QString StickerAnimator::animationPathFor(const QString &pngPath)
{
    QString path = pngPath;
    if (path.endsWith(".png")) path.chop(4);
    return path + ".webp";
}

void StickerAnimator::setImageView(bb::cascades::ImageView *view)
{
    if (m_view == view) return;
    m_view = view;
    emit imageViewChanged();
    load();
}

void StickerAnimator::setSource(const QString &source)
{
    // A ListView recycles its delegates: a new source is another sticker.
    if (m_source == source) return;
    m_source = source;
    emit sourceChanged();
    load();
}

void StickerAnimator::load()
{
    unload();
    if (!m_view) return;
    showFirstFrame();
    if (m_source.isEmpty()) return;

    QString png = m_source;
    if (png.startsWith("file://")) png = QUrl(png).toLocalFile();
    QFile file(animationPathFor(png));
    if (!file.open(QIODevice::ReadOnly)) return; // a still sticker
    m_data = file.readAll();

    WebPData webp;
    webp.bytes = reinterpret_cast<const uint8_t *>(m_data.constData());
    webp.size = m_data.size();
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) return;
    options.color_mode = MODE_rgbA; // = bb::PixelFormat::RGBA_Premultiplied
    options.use_threads = 0;
    m_decoder = WebPAnimDecoderNew(&webp, &options);
    WebPAnimInfo info;
    if (!m_decoder || !WebPAnimDecoderGetInfo(m_decoder, &info) || info.frame_count < 2) {
        unload();
        return;
    }
    m_width = info.canvas_width;
    m_height = info.canvas_height;
    emit animatedChanged();
    play();
}

void StickerAnimator::unload()
{
    m_timer.stop();
    const bool was = m_decoder != 0;
    if (m_decoder) WebPAnimDecoderDelete(m_decoder);
    m_decoder = 0;
    m_data.clear();
    if (was) emit animatedChanged();
}

void StickerAnimator::showFirstFrame()
{
    if (!m_view) return;
    m_view->setImageSource(m_source.isEmpty() ? QUrl() : QUrl(m_source));
}

void StickerAnimator::play()
{
    if (!m_decoder) return;
    m_loopsLeft = kLoops;
    WebPAnimDecoderReset(m_decoder);
    m_lastTimestamp = 0;
    if (m_appVisible) nextFrame();
}

void StickerAnimator::nextFrame()
{
    if (!m_decoder || !m_view) return;
    if (!WebPAnimDecoderHasMoreFrames(m_decoder)) {
        WebPAnimDecoderReset(m_decoder);
        m_lastTimestamp = 0;
        if (--m_loopsLeft <= 0) { // done: rest on the first frame
            int timestamp = 0;
            showNextFrame(&timestamp);
            return;
        }
    }
    int timestamp = 0;
    if (!showNextFrame(&timestamp)) return;
    // timestamp is where this frame ends, counted from the loop's start.
    const int delay = timestamp - m_lastTimestamp;
    m_lastTimestamp = timestamp;
    m_timer.start(delay < kMinFrameMs ? kMinFrameMs : delay);
}

// Decodes the next frame into the ImageView. (Not setImageSource() to go
// back to the PNG: after setImage() the ImageView still holds that same
// imageSource, so setting it again would change nothing.)
bool StickerAnimator::showNextFrame(int *timestamp)
{
    uint8_t *pixels = 0;
    if (!WebPAnimDecoderGetNext(m_decoder, &pixels, timestamp)) {
        unload();
        return false;
    }
    bb::ImageData frame(bb::PixelFormat::RGBA_Premultiplied, m_width, m_height);
    unsigned char *dst = frame.pixels();
    const int rowBytes = m_width * 4;
    for (int y = 0; y < m_height; ++y)
        memcpy(dst + y * frame.bytesPerLine(), pixels + y * rowBytes, rowBytes);
    m_view->setImage(bb::cascades::Image(frame));
    return true;
}

void StickerAnimator::onAppHidden()
{
    m_appVisible = false;
    m_timer.stop();
}

void StickerAnimator::onAppShown()
{
    const bool resume = !m_appVisible && m_decoder && m_loopsLeft > 0;
    m_appVisible = true;
    if (resume) nextFrame();
}
