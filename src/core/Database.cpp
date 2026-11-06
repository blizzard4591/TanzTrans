#include "Database.h"

#include "Log.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace tt {

namespace {

// Column order used by bindRecord() and readRecord(); "id" is handled separately.
const QStringList kColumns{
    QStringLiteral("path"),         QStringLiteral("remote_id"),     QStringLiteral("chash"),
    QStringLiteral("size"),         QStringLiteral("orig_size"),     QStringLiteral("mtime"),
    QStringLiteral("first_seen"),   QStringLiteral("last_seen"),     QStringLiteral("status"),
    QStringLiteral("priority"),     QStringLiteral("retries"),       QStringLiteral("not_before"),
    QStringLiteral("force"),        QStringLiteral("probed"),        QStringLiteral("has_video"),
    QStringLiteral("tagged"),       QStringLiteral("vcodec"),        QStringLiteral("width"),
    QStringLiteral("height"),       QStringLiteral("fps"),           QStringLiteral("hdr"),
    QStringLiteral("vbitrate"),     QStringLiteral("duration"),      QStringLiteral("threshold_bps"),
    QStringLiteral("cap_bps"),      QStringLiteral("local_source"),  QStringLiteral("local_output"),
    QStringLiteral("out_path"),     QStringLiteral("out_size"),      QStringLiteral("out_vbitrate"),
    QStringLiteral("backup_path"),  QStringLiteral("backup_expires"), QStringLiteral("log_path"),
    QStringLiteral("message"),      QStringLiteral("updated"),
};

QVariantList values(const FileRecord &r)
{
    return {r.path,         r.remoteId,     r.chash,       r.size,          r.origSize,     r.mtime,
            r.firstSeen,    r.lastSeen,     statusKey(r.status), r.priority, r.retries,     r.notBefore,
            int(r.force),   int(r.probed),  int(r.hasVideo), int(r.tagged), r.vcodec,       r.width,
            r.height,       r.fps,          r.hdr,         r.vbitrate,      r.duration,     r.thresholdBps,
            r.capBps,       r.localSource,  r.localOutput, r.outPath,       r.outSize,      r.outVbitrate,
            r.backupPath,   r.backupExpires, r.logPath,    r.message,       r.updated};
}

FileRecord readRecord(const QSqlQuery &q)
{
    FileRecord r;
    int i = 0;
    r.id = q.value(i++).toLongLong();
    r.path = q.value(i++).toString();
    r.remoteId = q.value(i++).toString();
    r.chash = q.value(i++).toString();
    r.size = q.value(i++).toLongLong();
    r.origSize = q.value(i++).toLongLong();
    r.mtime = q.value(i++).toLongLong();
    r.firstSeen = q.value(i++).toLongLong();
    r.lastSeen = q.value(i++).toLongLong();
    r.status = statusFromKey(q.value(i++).toString());
    r.priority = q.value(i++).toInt();
    r.retries = q.value(i++).toInt();
    r.notBefore = q.value(i++).toLongLong();
    r.force = q.value(i++).toInt() != 0;
    r.probed = q.value(i++).toInt() != 0;
    r.hasVideo = q.value(i++).toInt() != 0;
    r.tagged = q.value(i++).toInt() != 0;
    r.vcodec = q.value(i++).toString();
    r.width = q.value(i++).toInt();
    r.height = q.value(i++).toInt();
    r.fps = q.value(i++).toDouble();
    r.hdr = q.value(i++).toString();
    r.vbitrate = q.value(i++).toLongLong();
    r.duration = q.value(i++).toDouble();
    r.thresholdBps = q.value(i++).toLongLong();
    r.capBps = q.value(i++).toLongLong();
    r.localSource = q.value(i++).toString();
    r.localOutput = q.value(i++).toString();
    r.outPath = q.value(i++).toString();
    r.outSize = q.value(i++).toLongLong();
    r.outVbitrate = q.value(i++).toLongLong();
    r.backupPath = q.value(i++).toString();
    r.backupExpires = q.value(i++).toLongLong();
    r.logPath = q.value(i++).toString();
    r.message = q.value(i++).toString();
    r.updated = q.value(i++).toLongLong();
    return r;
}

constexpr auto kSchema = R"(
CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,
    remote_id TEXT, chash TEXT,
    size INTEGER DEFAULT 0, orig_size INTEGER DEFAULT 0, mtime INTEGER DEFAULT 0,
    first_seen INTEGER DEFAULT 0, last_seen INTEGER DEFAULT 0,
    status TEXT NOT NULL,
    priority INTEGER DEFAULT 0, retries INTEGER DEFAULT 0, not_before INTEGER DEFAULT 0, force INTEGER DEFAULT 0,
    probed INTEGER DEFAULT 0, has_video INTEGER DEFAULT 0, tagged INTEGER DEFAULT 0,
    vcodec TEXT, width INTEGER, height INTEGER, fps REAL, hdr TEXT, vbitrate INTEGER, duration REAL,
    threshold_bps INTEGER, cap_bps INTEGER,
    local_source TEXT, local_output TEXT, out_path TEXT, out_size INTEGER, out_vbitrate INTEGER,
    backup_path TEXT, backup_expires INTEGER, log_path TEXT, message TEXT, updated INTEGER
))";
// added in schema 2
constexpr auto kOldBackupsSchema = "CREATE TABLE IF NOT EXISTS old_backups (path TEXT PRIMARY KEY, expires INTEGER)";

} // namespace

Database::Database()
    : m_connection(QStringLiteral("tanztrans-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

Database::~Database()
{
    close();
}

Result<void> Database::open(const QString &path)
{
    close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(path);
    if (!db.open())
        return fail(QStringLiteral("cannot open database %1: %2").arg(path, db.lastError().text()));
    // Schema version: 0 = new file, 1 = files only, 2 = + old_backups (current). A schema change
    // bumps kSchemaVersion and migrates the older versions here before the version is written.
    // (QSQLITE runs one statement per exec.)
    constexpr int kSchemaVersion = 2;
    QString error;
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        const int version = q.exec(QStringLiteral("PRAGMA user_version")) && q.next() ? q.value(0).toInt() : 0;
        if (version > kSchemaVersion)
            error = QStringLiteral("database %1 was written by a newer TanzTrans (schema %2)").arg(path).arg(version);
        else if (!q.exec(QString::fromLatin1(kSchema)) || !q.exec(QString::fromLatin1(kOldBackupsSchema)))
            error = QStringLiteral("cannot create schema: %1").arg(q.lastError().text());
        else
            q.exec(QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion));
    }
    if (!error.isEmpty()) {
        close();
        return fail(error);
    }
    return {};
}

void Database::close()
{
    if (QSqlDatabase::contains(m_connection)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connection, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_connection);
    }
}

QList<FileRecord> Database::loadAll() const
{
    QList<FileRecord> result;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    if (!q.exec(QStringLiteral("SELECT id, %1 FROM files").arg(kColumns.join(QStringLiteral(", "))))) {
        logError(QStringLiteral("Database read failed: %1").arg(q.lastError().text()));
        return result;
    }
    while (q.next())
        result.append(readRecord(q));
    return result;
}

bool Database::save(FileRecord &record)
{
    record.updated = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(QSqlDatabase::database(m_connection));
    if (record.id == 0) {
        QStringList marks;
        marks.fill(QStringLiteral("?"), kColumns.size());
        q.prepare(QStringLiteral("INSERT INTO files (%1) VALUES (%2)")
                      .arg(kColumns.join(QStringLiteral(", ")), marks.join(QStringLiteral(", "))));
    } else {
        QStringList sets;
        for (const QString &c : kColumns)
            sets << c + QStringLiteral("=?");
        q.prepare(QStringLiteral("UPDATE files SET %1 WHERE id=?").arg(sets.join(QStringLiteral(", "))));
    }
    for (const QVariant &v : values(record))
        q.addBindValue(v);
    if (record.id != 0)
        q.addBindValue(record.id);
    if (!q.exec()) {
        logError(QStringLiteral("Database write failed for %1: %2").arg(record.path, q.lastError().text()));
        return false;
    }
    if (record.id == 0)
        record.id = q.lastInsertId().toLongLong();
    return true;
}

bool Database::remove(qint64 id)
{
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("DELETE FROM files WHERE id=?"));
    q.addBindValue(id);
    return q.exec();
}

void Database::addOldBackup(const QString &path, qint64 expires)
{
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO old_backups (path, expires) VALUES (?, ?)"));
    q.addBindValue(path);
    q.addBindValue(expires);
    if (!q.exec())
        logError(QStringLiteral("Database write failed for %1: %2").arg(path, q.lastError().text()));
}

QList<Database::OldBackup> Database::oldBackups() const
{
    QList<OldBackup> result;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    if (q.exec(QStringLiteral("SELECT path, expires FROM old_backups"))) {
        while (q.next())
            result.append({q.value(0).toString(), q.value(1).toLongLong()});
    }
    return result;
}

void Database::removeOldBackup(const QString &path)
{
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("DELETE FROM old_backups WHERE path=?"));
    q.addBindValue(path);
    q.exec();
}

} // namespace tt
