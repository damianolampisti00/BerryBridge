#ifndef WEBPIMAGE_HPP_
#define WEBPIMAGE_HPP_

#include <QByteArray>
#include <QImage>

// Decodes a WebP file (the format of WhatsApp stickers, received via the
// Beeper bridge) into a QImage. BB10 has no WebP codec (its img_codec_*.so
// cover bmp/gif/jpg/png/tga/tif only), so neither QImage nor a Cascades
// ImageView can open these files; libwebp 1.4.0 is vendored in
// third_party/libwebp (decoder + demuxer, see build-libwebp.sh there).
namespace WebPImage
{
    // True if data starts with a RIFF....WEBP header.
    bool isWebP(const QByteArray &data);

    // The image, or its first frame for an animated sticker; a null QImage
    // if data isn't a valid WebP file.
    QImage decodeFirstFrame(const QByteArray &data);
}

#endif /* WEBPIMAGE_HPP_ */
