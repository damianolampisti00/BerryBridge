#include "webpimage.hpp"

#include <webp/decode.h>
#include <webp/demux.h>

bool WebPImage::isWebP(const QByteArray &data)
{
    return data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP";
}

QImage WebPImage::decodeFirstFrame(const QByteArray &data, int *frameCount)
{
    if (!isWebP(data)) return QImage();

    // WebPAnimDecoder takes still images too (as a single frame), and for an
    // animated one it returns the composed canvas, not a bare sub-frame.
    WebPData webp;
    webp.bytes = reinterpret_cast<const uint8_t *>(data.constData());
    webp.size = data.size();
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) return QImage();
    // Premultiplied B,G,R,A bytes: QImage::Format_ARGB32_Premultiplied on a
    // little-endian CPU, so the pixels need no conversion.
    options.color_mode = MODE_bgrA;
    options.use_threads = 0;
    WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webp, &options);
    if (!decoder) return QImage();

    QImage image;
    WebPAnimInfo info;
    uint8_t *pixels = 0;
    int timestamp = 0;
    if (WebPAnimDecoderGetInfo(decoder, &info) && WebPAnimDecoderGetNext(decoder, &pixels, &timestamp)) {
        // copy(): the buffer belongs to the decoder, freed just below.
        image = QImage(pixels, info.canvas_width, info.canvas_height, info.canvas_width * 4,
                       QImage::Format_ARGB32_Premultiplied).copy();
        if (frameCount) *frameCount = info.frame_count;
    }
    WebPAnimDecoderDelete(decoder);
    return image;
}
