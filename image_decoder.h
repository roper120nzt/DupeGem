#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

namespace dg {

// Extensions supplied by Qt plus DupeGem's libheif and LibRaw decoders.
QStringList supportedImageExtensions();

// Reads an image and optionally scales it. A zero target dimension is derived
// from the source aspect ratio; QSize() leaves the decoded size unchanged.
QImage decodeImage(const QString& path, const QSize& target = QSize(),
                   Qt::AspectRatioMode mode = Qt::IgnoreAspectRatio,
                   QSize* sourceSize = nullptr);

// Obtains dimensions without developing RAW pixels or decoding the HEIF image.
QSize imageSourceSize(const QString& path);

} // namespace dg
