#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

namespace dg {

// Extensions supplied by Qt plus DupeGem's libheif and LibRaw decoders.
QStringList supportedImageExtensions();

// File types that can receive a lazy thumbnail from the Windows Shell.
QStringList supportedVideoExtensions();
bool isSupportedVideoFile(const QString& path);
bool isSupportedHeifFile(const QString& path);
bool isSupportedRawFile(const QString& path);

// Reads an image or requests a cached Windows video thumbnail, then optionally
// scales it. A zero target dimension is derived from the source aspect ratio;
// QSize() leaves decoded images unchanged and uses 320x180 for videos.
QImage decodeImage(const QString& path, const QSize& target = QSize(),
                   Qt::AspectRatioMode mode = Qt::IgnoreAspectRatio,
                   QSize* sourceSize = nullptr);

// Hash-oriented decoder. JPEG files use libjpeg-turbo DCT scaling and direct
// grayscale output; other formats use the normal decoder path.
QImage decodeHashImage(const QString& path, const QSize& target,
                       QSize* sourceSize = nullptr);

// Obtains dimensions without developing RAW pixels or decoding the HEIF image.
QSize imageSourceSize(const QString& path);

} // namespace dg
