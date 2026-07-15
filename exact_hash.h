#pragma once

#include <QByteArray>
#include <QString>

namespace dg {

inline constexpr int kBlake3DigestBytes = 32;
inline constexpr int kExactPrehashVersion = 3;

struct FileIdentity {
    quint64 volumeSerial{};
    QByteArray fileId;

    bool isValid() const { return volumeSerial != 0 && !fileId.isEmpty(); }
    QByteArray cacheKey() const;
};

struct ExactHashResult {
    quint64 sampleHash{};
    QByteArray blake3;
    qint64 bytesRead{};
    bool ok{};
};

enum class StorageClass { Unknown, Rotational, SolidState, Nvme };

FileIdentity fileIdentity(const QString& path);

// Fast first gate: first and final 16 KiB plus file size.
ExactHashResult quickSampleFile(const QString& path);

// Second gate: 16 KiB regions at 25%, 50%, and 75% plus file size.
ExactHashResult sampleFile(const QString& path);

// Computes a 256-bit BLAKE3 digest with the official SIMD-dispatched C code.
ExactHashResult hashFileBlake3(const QString& path);

// Performs a direct buffered comparison. This is the final authority before a
// file is included in an automatically deletable exact group.
bool filesByteEqual(const QString& left, const QString& right, qint64* bytesRead = nullptr);

StorageClass storageClass(const QString& path);

// Returns true only when Windows reports that the containing storage device
// incurs seek latency. Unknown/network storage returns false.
bool isRotationalStorage(const QString& path);

} // namespace dg
