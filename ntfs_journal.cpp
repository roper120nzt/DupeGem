#include "ntfs_journal.h"

#include "exact_hash.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

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

#ifdef Q_OS_WIN
class NativeHandle {
public:
    NativeHandle()=default;
    explicit NativeHandle(HANDLE value) : value_(value) {}
    ~NativeHandle() { if (valid()) CloseHandle(value_); }
    NativeHandle(const NativeHandle&)=delete;
    NativeHandle& operator=(const NativeHandle&)=delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(other.value_) {
        other.value_=INVALID_HANDLE_VALUE;
    }
    bool valid() const { return value_!=INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

QString windowsError(const QString& operation) {
    return QStringLiteral("%1 (Windows error %2)").arg(operation).arg(GetLastError());
}

NativeHandle openVolume(const QString& rootPath, QString* error=nullptr) {
    wchar_t mountPath[MAX_PATH]{};
    const std::wstring native=QDir::toNativeSeparators(
        QFileInfo(rootPath).absoluteFilePath()).toStdWString();
    if (!GetVolumePathNameW(native.c_str(),mountPath,MAX_PATH)) {
        if (error) *error=windowsError(QStringLiteral("Cannot locate the containing volume"));
        return {};
    }
    wchar_t volumeName[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(mountPath,volumeName,MAX_PATH)) {
        if (error) *error=windowsError(QStringLiteral("Cannot resolve the volume name"));
        return {};
    }
    std::wstring device(volumeName);
    while (!device.empty() && (device.back()==L'\\' || device.back()==L'/')) device.pop_back();
    HANDLE raw=CreateFileW(device.c_str(),GENERIC_READ,
                           FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,0,nullptr);
    // Keep a permission-free fallback so capability detection fails cleanly
    // instead of prompting for elevation.
    if (raw==INVALID_HANDLE_VALUE)
        raw=CreateFileW(device.c_str(),0,
                        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                        nullptr,OPEN_EXISTING,0,nullptr);
    NativeHandle handle(raw);
    if (!handle.valid() && error)
        *error=windowsError(QStringLiteral("Cannot open the NTFS volume journal"));
    return handle;
}

QByteArray identityKey(quint64 volumeSerial, quint64 reference) {
    QByteArray key(24,'\0');
    for (int i=0;i<8;++i) key[i]=char((volumeSerial>>(i*8))&0xffu);
    for (int i=0;i<8;++i) key[8+i]=char((reference>>(i*8))&0xffu);
    return key;
}

QString pathForReference(HANDLE volume, quint64 reference) {
    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize=sizeof(descriptor);
    descriptor.Type=FileIdType;
    descriptor.FileId.QuadPart=LONGLONG(reference);
    NativeHandle file(OpenFileById(volume,&descriptor,FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                   nullptr,FILE_FLAG_BACKUP_SEMANTICS));
    if (!file.valid()) return {};
    std::wstring buffer(32768,L'\0');
    const DWORD length=GetFinalPathNameByHandleW(file.get(),buffer.data(),DWORD(buffer.size()),
                                                 FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!length || length>=buffer.size()) return {};
    buffer.resize(length);
    QString path=QString::fromStdWString(buffer);
    if (path.startsWith(QStringLiteral("\\\\?\\UNC\\"),Qt::CaseInsensitive))
        path=QStringLiteral("\\\\")+path.mid(8);
    else if (path.startsWith(QStringLiteral("\\\\?\\"))) path=path.mid(4);
    return QDir::cleanPath(path);
}

bool isWithinRoot(const QString& path,const QString& root) {
    if (path.isEmpty()) return false;
    const QString cleanPath=QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString cleanRoot=QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    if (QString::compare(cleanPath,cleanRoot,Qt::CaseInsensitive)==0) return true;
    if (!cleanRoot.endsWith(QDir::separator())) cleanRoot+=QDir::separator();
    return cleanPath.startsWith(cleanRoot,Qt::CaseInsensitive);
}

#endif

} // namespace

QByteArray NtfsJournalCursor::serialize() const {
    QByteArray value;
    QDataStream stream(&value,QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << volumeSerial << journalId << nextUsn;
    return value;
}

NtfsJournalCursor NtfsJournalCursor::deserialize(const QByteArray& value) {
    NtfsJournalCursor cursor;
    if (value.size()!=24) return cursor;
    QDataStream stream(value);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream >> cursor.volumeSerial >> cursor.journalId >> cursor.nextUsn;
    if (stream.status()!=QDataStream::Ok) return {};
    return cursor;
}

NtfsJournalCursor currentNtfsJournal(const QString& rootPath, QString* error) {
#ifdef Q_OS_WIN
    QString openError;
    NativeHandle volume=openVolume(rootPath,&openError);
    if (!volume.valid()) { if (error) *error=openError; return {}; }
    USN_JOURNAL_DATA journal{};
    DWORD returned=0;
    if (!DeviceIoControl(volume.get(),FSCTL_QUERY_USN_JOURNAL,nullptr,0,
                         &journal,sizeof(journal),&returned,nullptr)) {
        if (error) *error=windowsError(QStringLiteral("NTFS change journal unavailable"));
        return {};
    }
    NtfsJournalCursor cursor;
    cursor.volumeSerial=fileIdentity(rootPath).volumeSerial;
    cursor.journalId=journal.UsnJournalID;
    cursor.nextUsn=journal.NextUsn;
    return cursor;
#else
    Q_UNUSED(rootPath);
    if (error) *error=QStringLiteral("NTFS journal support is available only on Windows.");
    return {};
#endif
}

NtfsChangeSet readNtfsChanges(const QString& rootPath,const NtfsJournalCursor& from) {
    NtfsChangeSet result;
#ifdef Q_OS_WIN
    QString openError;
    NativeHandle volume=openVolume(rootPath,&openError);
    if (!volume.valid()) { result.error=openError; return result; }
    USN_JOURNAL_DATA journal{};
    DWORD returned=0;
    if (!DeviceIoControl(volume.get(),FSCTL_QUERY_USN_JOURNAL,nullptr,0,
                         &journal,sizeof(journal),&returned,nullptr)) {
        result.error=windowsError(QStringLiteral("NTFS change journal unavailable"));
        return result;
    }
    result.cursor.volumeSerial=fileIdentity(rootPath).volumeSerial;
    result.cursor.journalId=journal.UsnJournalID;
    result.cursor.nextUsn=journal.NextUsn;
    if (!from.isValid() || result.cursor.volumeSerial!=from.volumeSerial
        || result.cursor.journalId!=from.journalId || from.nextUsn<journal.LowestValidUsn
        || from.nextUsn>journal.NextUsn) {
        result.error=QStringLiteral("NTFS journal baseline expired or belongs to another volume.");
        return result;
    }

    READ_USN_JOURNAL_DATA request{};
    request.StartUsn=from.nextUsn;
    request.ReasonMask=0xffffffffu;
    request.ReturnOnlyOnClose=FALSE;
    request.Timeout=0;
    request.BytesToWaitFor=0;
    request.UsnJournalID=from.journalId;
    QByteArray buffer(1024*1024,Qt::Uninitialized);
    QHash<quint64,NtfsChange> changes;
    while (request.StartUsn<journal.NextUsn) {
        returned=0;
        if (!DeviceIoControl(volume.get(),FSCTL_READ_UNPRIVILEGED_USN_JOURNAL,&request,sizeof(request),
                             buffer.data(),DWORD(buffer.size()),&returned,nullptr)) {
            result.error=windowsError(QStringLiteral("Cannot read NTFS journal changes"));
            return result;
        }
        if (returned<=sizeof(USN)) break;
        const USN next=*reinterpret_cast<const USN*>(buffer.constData());
        DWORD offset=sizeof(USN);
        while (offset+sizeof(USN_RECORD)<=returned) {
            const auto* record=reinterpret_cast<const USN_RECORD*>(buffer.constData()+offset);
            if (record->RecordLength<sizeof(USN_RECORD) || offset+record->RecordLength>returned) break;
            NtfsChange& change=changes[record->FileReferenceNumber];
            change.fileIdentity=identityKey(result.cursor.volumeSerial,record->FileReferenceNumber);
            change.usn=std::max<qint64>(change.usn,record->Usn);
            change.reasons|=record->Reason;
            change.directory=(record->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
            if (change.previousName.isEmpty()
                && (record->Reason&(USN_REASON_RENAME_OLD_NAME|USN_REASON_FILE_DELETE))
                && record->FileNameLength>0
                && DWORD(record->FileNameOffset)+DWORD(record->FileNameLength)<=record->RecordLength) {
                change.previousName=QString::fromWCharArray(
                    reinterpret_cast<const wchar_t*>(
                        reinterpret_cast<const uchar*>(record)+record->FileNameOffset),
                    record->FileNameLength/int(sizeof(wchar_t)));
                change.previousParentReference=record->ParentFileReferenceNumber;
            }
            offset+=record->RecordLength;
        }
        if (next<=request.StartUsn) break;
        request.StartUsn=next;
    }

    result.changes.reserve(size_t(changes.size()));
    for (auto it=changes.begin();it!=changes.end();++it) {
        NtfsChange change=it.value();
        change.currentPath=pathForReference(volume.get(),it.key());
        if (!change.previousName.isEmpty() && change.previousParentReference) {
            const QString parent=pathForReference(volume.get(),change.previousParentReference);
            if (!parent.isEmpty()) change.previousPath=QDir(parent).filePath(change.previousName);
        }
        change.deleted=change.currentPath.isEmpty();
        change.contentChanged=(change.reasons&(USN_REASON_DATA_OVERWRITE
                                                |USN_REASON_DATA_EXTEND
                                                |USN_REASON_DATA_TRUNCATION
                                                |USN_REASON_NAMED_DATA_OVERWRITE
                                                |USN_REASON_NAMED_DATA_EXTEND
                                                |USN_REASON_NAMED_DATA_TRUNCATION
                                                |USN_REASON_FILE_CREATE
                                                |USN_REASON_STREAM_CHANGE))!=0;
        if (change.directory && (isWithinRoot(change.currentPath,rootPath)
                                 || isWithinRoot(change.previousPath,rootPath))
            && (change.reasons&(USN_REASON_RENAME_OLD_NAME
                                |USN_REASON_RENAME_NEW_NAME
                                |USN_REASON_FILE_DELETE)))
            result.requiresFullScan=true;
        result.changes.push_back(std::move(change));
    }
    result.complete=true;
#else
    Q_UNUSED(rootPath); Q_UNUSED(from);
    result.error=QStringLiteral("NTFS journal support is available only on Windows.");
#endif
    return result;
}

} // namespace dg
