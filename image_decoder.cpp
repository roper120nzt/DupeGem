#include "image_decoder.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QSemaphore>
#include <QTransform>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libheif/heif.h>
#include <libraw/libraw.h>

namespace dg {
namespace {

const QStringList& heifExtensions() {
    static const QStringList extensions{QStringLiteral("heic"), QStringLiteral("heif"),
                                        QStringLiteral("hif")};
    return extensions;
}

const QStringList& rawExtensions() {
    // LibRaw recognizes formats from these camera families. Detection still
    // happens from the file contents, so an extension match is not treated as
    // proof that a file is valid RAW data.
    static const QStringList extensions{
        QStringLiteral("3fr"), QStringLiteral("ari"), QStringLiteral("arw"),
        QStringLiteral("bay"), QStringLiteral("cap"), QStringLiteral("cr2"),
        QStringLiteral("cr3"), QStringLiteral("crw"), QStringLiteral("dcr"),
        QStringLiteral("dcs"), QStringLiteral("dng"), QStringLiteral("drf"),
        QStringLiteral("eip"), QStringLiteral("erf"), QStringLiteral("fff"),
        QStringLiteral("gpr"), QStringLiteral("iiq"), QStringLiteral("k25"),
        QStringLiteral("kdc"), QStringLiteral("mdc"), QStringLiteral("mef"),
        QStringLiteral("mos"), QStringLiteral("mrw"), QStringLiteral("nef"),
        QStringLiteral("nrw"), QStringLiteral("orf"), QStringLiteral("pef"),
        QStringLiteral("ptx"), QStringLiteral("pxn"), QStringLiteral("r3d"),
        QStringLiteral("raf"), QStringLiteral("raw"), QStringLiteral("rw2"),
        QStringLiteral("rwl"), QStringLiteral("rwz"), QStringLiteral("sr2"),
        QStringLiteral("srf"), QStringLiteral("srw"), QStringLiteral("x3f")};
    return extensions;
}

QString suffixOf(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

bool isHeif(const QString& path) { return heifExtensions().contains(suffixOf(path)); }
bool isRaw(const QString& path) { return rawExtensions().contains(suffixOf(path)); }

QSize orientedSize(QSize size, int flip) {
    if (flip == 5 || flip == 6) size.transpose();
    return size;
}

QImage applyRawOrientation(QImage image, int flip) {
    if (image.isNull()) return image;
    int degrees = 0;
    if (flip == 3) degrees = 180;
    else if (flip == 5) degrees = -90;
    else if (flip == 6) degrees = 90;
    return degrees ? image.transformed(QTransform().rotate(degrees)) : image;
}

QSize requestedSize(const QSize& source, const QSize& target, Qt::AspectRatioMode mode) {
    if (!source.isValid() || target.isEmpty()) return target;
    if (target.width() <= 0 && target.height() > 0)
        return QSize(std::max(1, qRound(double(source.width()) * target.height() / source.height())),
                     target.height());
    if (target.height() <= 0 && target.width() > 0)
        return QSize(target.width(),
                     std::max(1, qRound(double(source.height()) * target.width() / source.width())));
    return mode == Qt::IgnoreAspectRatio ? target : source.scaled(target, mode);
}

QImage finishScale(QImage image, const QSize& target, Qt::AspectRatioMode mode) {
    if (image.isNull() || target.isEmpty()) return image;
    const QSize wanted = requestedSize(image.size(), target, mode);
    if (!wanted.isValid() || wanted == image.size()) return image;
    return image.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

class HeifContext {
public:
    HeifContext() : value(heif_context_alloc()) {}
    ~HeifContext() { if (value) heif_context_free(value); }
    heif_context* value{};
};

class HeifHandle {
public:
    HeifHandle() = default;
    HeifHandle(const HeifHandle&) = delete;
    HeifHandle& operator=(const HeifHandle&) = delete;
    HeifHandle(HeifHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    HeifHandle& operator=(HeifHandle&& other) noexcept {
        if (this != &other) {
            if (value) heif_image_handle_release(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~HeifHandle() { if (value) heif_image_handle_release(value); }
    heif_image_handle* value{};
};

class HeifImage {
public:
    ~HeifImage() { if (value) heif_image_release(value); }
    heif_image* value{};
};

struct HeifFileReader {
    QFile file;
    qint64 size{};
};

int64_t heifPosition(void* user) {
    return static_cast<HeifFileReader*>(user)->file.pos();
}

int heifRead(void* data, size_t size, void* user) {
    if (size > size_t(std::numeric_limits<qint64>::max())) return 1;
    return static_cast<HeifFileReader*>(user)->file.read(static_cast<char*>(data), qint64(size))
                   == qint64(size) ? 0 : 1;
}

int heifSeek(int64_t position, void* user) {
    return position >= 0 && static_cast<HeifFileReader*>(user)->file.seek(position) ? 0 : 1;
}

heif_reader_grow_status heifWaitForSize(int64_t target, void* user) {
    if (target < 0) return heif_reader_grow_status_error;
    return target <= static_cast<HeifFileReader*>(user)->size
               ? heif_reader_grow_status_size_reached
               : heif_reader_grow_status_size_beyond_eof;
}

const heif_reader& qtHeifReader() {
    static const heif_reader reader{1, heifPosition, heifRead, heifSeek, heifWaitForSize,
                                    nullptr, nullptr, nullptr, nullptr};
    return reader;
}

bool openHeif(const QString& path, HeifFileReader& reader, HeifContext& context,
              HeifHandle& primary) {
    reader.file.setFileName(path);
    if (!reader.file.open(QIODevice::ReadOnly) || !context.value) return false;
    reader.size = reader.file.size();
    heif_error error = heif_context_read_from_reader(context.value, &qtHeifReader(),
                                                     &reader, nullptr);
    if (error.code != heif_error_Ok) return false;
    error = heif_context_get_primary_image_handle(context.value, &primary.value);
    return error.code == heif_error_Ok && primary.value;
}

HeifHandle bestHeifHandle(heif_image_handle* primary, const QSize& target) {
    HeifHandle selected;
    const int count = heif_image_handle_get_number_of_thumbnails(primary);
    if (count <= 0) return selected;
    std::vector<heif_item_id> ids;
    ids.resize(size_t(count));
    const int found = heif_image_handle_get_list_of_thumbnail_IDs(primary, ids.data(), count);
    qint64 bestArea = std::numeric_limits<qint64>::max();
    qint64 largestArea = -1;
    heif_item_id bestId = 0;
    heif_item_id largestId = 0;
    for (int i = 0; i < found; ++i) {
        HeifHandle candidate;
        if (heif_image_handle_get_thumbnail(primary, ids[size_t(i)], &candidate.value).code
            != heif_error_Ok) continue;
        const int width = heif_image_handle_get_width(candidate.value);
        const int height = heif_image_handle_get_height(candidate.value);
        const qint64 area = qint64(width) * height;
        if (area > largestArea) { largestArea = area; largestId = ids[size_t(i)]; }
        const bool sufficient = target.isEmpty()
            || ((target.width() <= 0 || width >= target.width())
                && (target.height() <= 0 || height >= target.height()));
        if (sufficient && area < bestArea) { bestArea = area; bestId = ids[size_t(i)]; }
    }
    const heif_item_id id = bestId ? bestId : largestId;
    if (id) heif_image_handle_get_thumbnail(primary, id, &selected.value);
    return selected;
}

QImage decodeHeif(const QString& path, const QSize& target, Qt::AspectRatioMode mode,
                  QSize* sourceSize, bool pixels) {
    HeifFileReader reader;
    HeifContext context;
    HeifHandle primary;
    if (!openHeif(path, reader, context, primary)) return {};
    const QSize source(heif_image_handle_get_width(primary.value),
                       heif_image_handle_get_height(primary.value));
    if (sourceSize) *sourceSize = source;
    if (!pixels) return {};

    const QSize wanted = requestedSize(source, target, mode);
    HeifHandle thumbnail = bestHeifHandle(primary.value, wanted);
    heif_image_handle* decodeHandle = thumbnail.value ? thumbnail.value : primary.value;

    HeifImage decoded;
    const heif_error error = heif_decode_image(decodeHandle, &decoded.value,
                                               heif_colorspace_RGB,
                                               heif_chroma_interleaved_RGBA, nullptr);
    if (error.code != heif_error_Ok || !decoded.value) return {};
    const int width = heif_image_get_width(decoded.value, heif_channel_interleaved);
    const int height = heif_image_get_height(decoded.value, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(decoded.value,
                                                         heif_channel_interleaved, &stride);
    if (!plane || width <= 0 || height <= 0 || stride < width * 4) return {};
    QImage image(plane, width, height, stride, QImage::Format_RGBA8888);
    return finishScale(image.copy(), target, mode);
}

QImage processedRawImage(libraw_processed_image_t* processed,
                         const QSize& jpegTarget = QSize()) {
    if (!processed || !processed->data_size) return {};
    if (processed->type == LIBRAW_IMAGE_JPEG) {
        if (processed->data_size > unsigned(std::numeric_limits<int>::max())) return {};
        QByteArray bytes = QByteArray::fromRawData(
            reinterpret_cast<const char*>(processed->data), int(processed->data_size));
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::ReadOnly)) return {};
        QImageReader reader(&buffer, "JPEG");
        if (jpegTarget.isValid()) reader.setScaledSize(jpegTarget);
        return reader.read();
    }
    if (processed->type != LIBRAW_IMAGE_BITMAP || processed->bits != 8
        || processed->width == 0 || processed->height == 0) return {};
    QImage::Format format = QImage::Format_Invalid;
    if (processed->colors == 1) format = QImage::Format_Grayscale8;
    else if (processed->colors == 3) format = QImage::Format_RGB888;
    else if (processed->colors == 4) format = QImage::Format_RGBA8888;
    if (format == QImage::Format_Invalid) return {};
    const qsizetype stride = qsizetype(processed->width) * processed->colors;
    if (stride <= 0 || quint64(stride) * processed->height > processed->data_size) return {};
    return QImage(processed->data, processed->width, processed->height, stride, format).copy();
}

class RawProcessedImage {
public:
    ~RawProcessedImage() { if (value) LibRaw::dcraw_clear_mem(value); }
    libraw_processed_image_t* value{};
};

QSemaphore& fullDecodeSlots() {
    static QSemaphore semaphore(2);
    return semaphore;
}

class DecodeSlot {
public:
    DecodeSlot() { fullDecodeSlots().acquire(); }
    ~DecodeSlot() { fullDecodeSlots().release(); }
};

bool openRaw(const QString& path, LibRaw& raw) {
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return raw.open_file(native.c_str()) == LIBRAW_SUCCESS;
#else
    const QByteArray native = QFile::encodeName(path);
    return raw.open_file(native.constData()) == LIBRAW_SUCCESS;
#endif
}

QImage decodeRaw(const QString& path, const QSize& target, Qt::AspectRatioMode mode,
                 QSize* sourceSize, bool pixels) {
    LibRaw raw;
    if (!openRaw(path, raw)) return {};
    const int flip = raw.imgdata.sizes.flip;
    const QSize source = orientedSize(QSize(raw.imgdata.sizes.width,
                                            raw.imgdata.sizes.height), flip);
    if (sourceSize) *sourceSize = source;
    if (!pixels) return {};

    QImage image;
    if (raw.unpack_thumb() == LIBRAW_SUCCESS) {
        int error = LIBRAW_SUCCESS;
        RawProcessedImage preview;
        preview.value = raw.dcraw_make_mem_thumb(&error);
        QSize previewTarget = requestedSize(source, target, mode);
        if (flip == 5 || flip == 6) previewTarget.transpose();
        if (error == LIBRAW_SUCCESS)
            image = processedRawImage(preview.value, previewTarget);
    }
    if (!image.isNull())
        return finishScale(applyRawOrientation(std::move(image), flip), target, mode);

    // Some RAW files have no usable embedded preview. Developing is expensive,
    // so cap concurrent fallbacks even when the normal hash pool is larger.
    DecodeSlot slot;
    raw.recycle();
    if (!openRaw(path, raw)) return {};
    raw.imgdata.params.output_bps = 8;
    raw.imgdata.params.output_color = 1; // sRGB
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.half_size = 1;
    if (raw.unpack() != LIBRAW_SUCCESS || raw.dcraw_process() != LIBRAW_SUCCESS) return {};
    int error = LIBRAW_SUCCESS;
    RawProcessedImage developed;
    developed.value = raw.dcraw_make_mem_image(&error);
    if (error != LIBRAW_SUCCESS) return {};
    image = processedRawImage(developed.value);
    // dcraw_process applies the RAW orientation itself.
    return finishScale(std::move(image), target, mode);
}

} // namespace

QStringList supportedImageExtensions() {
    QStringList extensions;
    for (const QByteArray& format : QImageReader::supportedImageFormats())
        extensions << QString::fromLatin1(format).toLower();
    extensions << heifExtensions() << rawExtensions();
    extensions.removeDuplicates();
    std::sort(extensions.begin(), extensions.end(), [](const QString& left, const QString& right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    return extensions;
}

QImage decodeImage(const QString& path, const QSize& target, Qt::AspectRatioMode mode,
                   QSize* sourceSize) {
    if (sourceSize) *sourceSize = {};
    if (isHeif(path)) return decodeHeif(path, target, mode, sourceSize, true);
    if (isRaw(path)) return decodeRaw(path, target, mode, sourceSize, true);

    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return {};
    QSize source = reader.size();
    const bool transposed = reader.transformation() & QImageIOHandler::TransformationRotate90;
    if (transposed)
        source.transpose();
    if (sourceSize) *sourceSize = source;
    QSize wanted = requestedSize(source, target, mode);
    // QImageReader scales before applying EXIF rotation, so swap the decoder
    // request for 90/270-degree images to obtain the intended final size.
    if (transposed) wanted.transpose();
    if (wanted.isValid()) reader.setScaledSize(wanted);
    return reader.read();
}

QSize imageSourceSize(const QString& path) {
    QSize source;
    if (isHeif(path)) { decodeHeif(path, {}, Qt::IgnoreAspectRatio, &source, false); return source; }
    if (isRaw(path)) { decodeRaw(path, {}, Qt::IgnoreAspectRatio, &source, false); return source; }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return {};
    source = reader.size();
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        source.transpose();
    return source;
}

} // namespace dg
