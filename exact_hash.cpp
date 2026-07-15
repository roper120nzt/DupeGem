#include "exact_hash.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <array>
#include <cstring>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

extern "C" {
#include "blake3.h"
}

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace dg {
namespace {

constexpr qint64 kSampleBytes = 16 * 1024;
constexpr qsizetype kStreamBuffer = 4 * 1024 * 1024;

QByteArray littleEndian64(quint64 value) {
    QByteArray bytes(sizeof(value), Qt::Uninitialized);
    for (int i = 0; i < int(sizeof(value)); ++i)
        bytes[i] = char((value >> (i * 8)) & 0xffu);
    return bytes;
}

#ifdef Q_OS_WIN
class NativeHandle {
public:
    explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~NativeHandle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

NativeHandle openAttributes(const QString& path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return NativeHandle(CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr));
}
#endif

} // namespace

QByteArray FileIdentity::cacheKey() const {
    if (!isValid()) return {};
    return littleEndian64(volumeSerial) + fileId;
}

FileIdentity fileIdentity(const QString& path) {
#ifdef Q_OS_WIN
    NativeHandle handle = openAttributes(path);
    if (!handle.valid()) return {};

    FILE_ID_INFO info{};
    if (GetFileInformationByHandleEx(handle.get(), FileIdInfo, &info, sizeof(info))) {
        FileIdentity identity;
        identity.volumeSerial = info.VolumeSerialNumber;
        identity.fileId = QByteArray(reinterpret_cast<const char*>(info.FileId.Identifier),
                                     sizeof(info.FileId.Identifier));
        return identity;
    }

    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!GetFileInformationByHandle(handle.get(), &legacy)) return {};
    FileIdentity identity;
    identity.volumeSerial = legacy.dwVolumeSerialNumber;
    const quint64 index = (quint64(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
    identity.fileId = littleEndian64(index);
    return identity;
#else
    Q_UNUSED(path);
    return {};
#endif
}

static ExactHashResult sampleRegions(const QString& path,const std::vector<qint64>& requestedOffsets) {
    ExactHashResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const qint64 size = file.size();
    if (size < 0) return result;

    XXH3_state_t* state = XXH3_createState();
    if (!state) return result;
    if (XXH3_64bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        return result;
    }
    const QByteArray sizeBytes = littleEndian64(quint64(size));
    XXH3_64bits_update(state, sizeBytes.constData(), size_t(sizeBytes.size()));

    const qint64 length=std::min(size,kSampleBytes);
    std::vector<qint64> offsets=requestedOffsets;
    std::sort(offsets.begin(),offsets.end());
    QByteArray buffer(qsizetype(length), Qt::Uninitialized);
    qint64 previous = -1;
    for (qint64 offset : offsets) {
        if (offset == previous) continue;
        previous = offset;
        if (!file.seek(offset)) { XXH3_freeState(state); return {}; }
        const qint64 count = file.read(buffer.data(), length);
        if (count != length) { XXH3_freeState(state); return {}; }
        const QByteArray offsetBytes = littleEndian64(quint64(offset));
        XXH3_64bits_update(state, offsetBytes.constData(), size_t(offsetBytes.size()));
        XXH3_64bits_update(state, buffer.constData(), size_t(count));
        result.bytesRead += count;
    }
    result.sampleHash = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    result.ok = true;
    return result;
}

ExactHashResult quickSampleFile(const QString& path) {
    const qint64 size=QFileInfo(path).size();
    if (size<0) return {};
    const qint64 length=std::min(size,kSampleBytes);
    return sampleRegions(path,{0,std::max<qint64>(0,size-length)});
}

ExactHashResult sampleFile(const QString& path) {
    const qint64 size=QFileInfo(path).size();
    if (size<0) return {};
    const qint64 length=std::min(size,kSampleBytes);
    const qint64 last=std::max<qint64>(0,size-length);
    return sampleRegions(path,{last/4,last/2,(last*3)/4});
}

ExactHashResult hashFileBlake3(const QString& path) {
    ExactHashResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return result;

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    QByteArray buffer(kStreamBuffer, Qt::Uninitialized);
    while (true) {
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0) return {};
        if (count == 0) break;
        blake3_hasher_update(&hasher, buffer.constData(), size_t(count));
        result.bytesRead += count;
    }
    result.blake3.resize(BLAKE3_OUT_LEN);
    blake3_hasher_finalize(&hasher,
                           reinterpret_cast<uint8_t*>(result.blake3.data()),
                           size_t(result.blake3.size()));
    result.ok = true;
    return result;
}

bool filesByteEqual(const QString& left, const QString& right, qint64* bytesRead) {
    if (bytesRead) *bytesRead = 0;
    QFileInfo leftInfo(left), rightInfo(right);
    if (leftInfo.size() != rightInfo.size() || leftInfo.size() < 0) return false;
    if (leftInfo.absoluteFilePath() == rightInfo.absoluteFilePath()) return true;

    QFile a(left), b(right);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly)) return false;
    QByteArray leftBuffer(kStreamBuffer, Qt::Uninitialized);
    QByteArray rightBuffer(kStreamBuffer, Qt::Uninitialized);
    while (true) {
        const qint64 leftCount = a.read(leftBuffer.data(), leftBuffer.size());
        const qint64 rightCount = b.read(rightBuffer.data(), rightBuffer.size());
        if (leftCount < 0 || rightCount < 0 || leftCount != rightCount) return false;
        if (bytesRead) *bytesRead += leftCount + rightCount;
        if (leftCount == 0) return true;
        if (std::memcmp(leftBuffer.constData(), rightBuffer.constData(), size_t(leftCount)) != 0)
            return false;
    }
}

StorageClass storageClass(const QString& path) {
#ifdef Q_OS_WIN
    wchar_t volumePath[MAX_PATH]{};
    const std::wstring native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
    if (!GetVolumePathNameW(native.c_str(), volumePath, MAX_PATH)) return StorageClass::Unknown;
    std::wstring device(volumePath);
    if (device.size() < 2 || device[1] != L':') return StorageClass::Unknown;
    device = L"\\\\.\\" + device.substr(0, 2);
    NativeHandle handle(CreateFileW(device.c_str(), 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) return StorageClass::Unknown;

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query), &descriptor, sizeof(descriptor),
                         &returned, nullptr)) return StorageClass::Unknown;
    if (descriptor.IncursSeekPenalty) return StorageClass::Rotational;

    std::array<uchar,1024> deviceBuffer{};
    query={}; query.PropertyId=StorageDeviceProperty; query.QueryType=PropertyStandardQuery;
    if (DeviceIoControl(handle.get(),IOCTL_STORAGE_QUERY_PROPERTY,
                        &query,sizeof(query),deviceBuffer.data(),DWORD(deviceBuffer.size()),
                        &returned,nullptr)) {
        const auto* deviceDescriptor=
            reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(deviceBuffer.data());
        if (deviceDescriptor->BusType==BusTypeNvme) return StorageClass::Nvme;
    }
    return StorageClass::SolidState;
#else
    Q_UNUSED(path);
    return StorageClass::Unknown;
#endif
}

bool isRotationalStorage(const QString& path) {
    return storageClass(path)==StorageClass::Rotational;
}

} // namespace dg
