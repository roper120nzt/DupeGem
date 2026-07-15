#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace dg {

struct NtfsJournalCursor {
    quint64 volumeSerial{};
    quint64 journalId{};
    qint64 nextUsn{};

    bool isValid() const { return volumeSerial && journalId && nextUsn >= 0; }
    QByteArray serialize() const;
    static NtfsJournalCursor deserialize(const QByteArray& value);
};

struct NtfsChange {
    QByteArray fileIdentity;
    QString currentPath;
    QString previousPath;
    QString previousName;
    quint64 previousParentReference{};
    qint64 usn{};
    quint32 reasons{};
    bool deleted{};
    bool directory{};
    bool contentChanged{};
};

struct NtfsChangeSet {
    NtfsJournalCursor cursor;
    std::vector<NtfsChange> changes;
    QString error;
    bool complete{};
    bool requiresFullScan{};
};

// Invalid cursors mean non-NTFS, network, inaccessible, or journal-disabled.
// Callers must transparently fall back to normal enumeration.
NtfsJournalCursor currentNtfsJournal(const QString& rootPath, QString* error = nullptr);

// Resolves surviving changed file IDs to paths. Directory renames request a
// full scan because descendants change paths without individual USN records.
NtfsChangeSet readNtfsChanges(const QString& rootPath, const NtfsJournalCursor& from);

} // namespace dg
