#pragma once

#include "FileRecord.h"
#include "Result.h"

#include <QList>
#include <QString>

namespace tt {

/// SQLite persistence of FileRecords (one row per remote path).
class Database
{
public:
    Database();
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    Result<void> open(const QString &path);
    void close();

    QList<FileRecord> loadAll() const;
    bool save(FileRecord &record); ///< insert (sets id) or update
    bool remove(qint64 id);

    /// Backups of earlier versions of a path (the file was replaced and later uploaded again):
    /// no record points to them any more, but they are kept until their own expiry.
    struct OldBackup
    {
        QString path;
        qint64 expires = 0;
    };
    void addOldBackup(const QString &path, qint64 expires);
    QList<OldBackup> oldBackups() const;
    void removeOldBackup(const QString &path);

private:
    QString m_connection;
};

} // namespace tt
