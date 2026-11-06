#include "JobManager.h"

#include "Encoder.h"
#include "FfmpegArgs.h"
#include "Log.h"
#include "Policy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>
#include <QThread>

#include <algorithm>
#include <memory>

namespace tt {

namespace {
qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

int statusRank(FileStatus s)
{
    switch (s) {
    case FileStatus::Encoding:
    case FileStatus::Uploading:
    case FileStatus::Downloading:
    case FileStatus::Probing: return 0;
    case FileStatus::Encoded: return 1;
    case FileStatus::Downloaded: return 2;
    case FileStatus::Queued: return 3;
    case FileStatus::Pending: return 4;
    case FileStatus::Failed: return 5;
    default: return 6;
    }
}

bool queueBefore(const FileRecord &a, const FileRecord &b)
{
    if (a.priority != b.priority)
        return a.priority > b.priority;
    if (a.mtime != b.mtime)
        return a.mtime > b.mtime; // newest first
    return a.path < b.path;
}

bool expectsRemote(const FileRecord &r)
{
    switch (r.status) {
    case FileStatus::Pending:
    case FileStatus::SmallEnough:
    case FileStatus::AlreadyDone:
    case FileStatus::NotVideo:
    case FileStatus::Queued:
    case FileStatus::Downloaded:
    case FileStatus::NoGain:
    case FileStatus::Failed:
    case FileStatus::Skipped:
    case FileStatus::DoneLocal:
    case FileStatus::Output: return true;
    case FileStatus::Done: return r.outPath == r.path;
    default: return false;
    }
}

void appendToLog(const QString &logPath, const QString &text)
{
    if (logPath.isEmpty())
        return;
    QFile f(logPath);
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((text + QLatin1Char('\n')).toUtf8());
}

void setLocalMtime(const QString &path, qint64 mtime)
{
    QFile f(path);
    if (mtime > 0 && f.open(QIODevice::Append))
        f.setFileTime(QDateTime::fromSecsSinceEpoch(mtime), QFileDevice::FileModificationTime);
}

QString uniqueLocalPath(const QString &path)
{
    if (!QFile::exists(path))
        return path;
    const QFileInfo fi(path);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName()
           + QDateTime::currentDateTime().toString(QStringLiteral("_yyyyMMdd-HHmmss.")) + fi.suffix();
}

/// The upload's name until it replaces the original.
QString tempName(const QString &name)
{
    return name + QStringLiteral(".tanztrans-part");
}

/// The original's name while the new file takes its place (same-name replacement only).
QString asideName(const QString &name)
{
    return name + QStringLiteral(".tanztrans-orig");
}

/// Names the transcode may get on HiDrive: an .mp4 keeps its name, anything else becomes
/// <stem>.mp4, or <stem>_tt.mp4 if that is taken.
QStringList finalNameCandidates(const QString &name)
{
    const QFileInfo fi(name);
    if (fi.suffix().compare(QLatin1String("mp4"), Qt::CaseInsensitive) == 0)
        return {name};
    return {fi.completeBaseName() + QStringLiteral(".mp4"), fi.completeBaseName() + QStringLiteral("_tt.mp4")};
}

/// The remote file is still the one that was downloaded and transcoded.
bool unchangedOnRemote(const FileRecord &r, const RemoteEntry &e)
{
    const qint64 expectedSize = r.origSize > 0 ? r.origSize : r.size;
    return e.size == expectedSize && (r.chash.isEmpty() || e.chash.isEmpty() || e.chash == r.chash);
}

/// "folder/clip.mov" -> "folder/clip.mp4" (read-only mode output, relative to the output folder).
QString outputRelativePath(const QString &base, const QString &path)
{
    const QFileInfo fi(relativeToBase(base, path));
    const QString name = fi.completeBaseName() + QStringLiteral(".mp4");
    return fi.path() == QLatin1String(".") ? name : fi.path() + QLatin1Char('/') + name;
}

/// Moves a file on a worker thread (a move to another drive is a copy of several GB) and
/// calls `done` with an error text, or an empty one, on `context`'s thread.
void moveFileAsync(const QString &source, const QString &target, QObject *context,
                   std::function<void(const QString &error)> done)
{
    auto error = std::make_shared<QString>();
    QThread *thread = QThread::create([source, target, error] {
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile file(source);
        if (!file.rename(target))
            *error = file.errorString();
    });
    QObject::connect(thread, &QThread::finished, context, [error, done = std::move(done)] { done(*error); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

/// A record left in a running state by a quit or a crash goes back to the step before.
bool recoverInterrupted(FileRecord &r)
{
    switch (r.status) {
    case FileStatus::Probing: r.status = FileStatus::Pending; return true;
    case FileStatus::Downloading: r.status = FileStatus::Queued; return true;
    case FileStatus::Encoding: // a half-written output is useless
        QFile::remove(r.localOutput);
        r.localOutput.clear();
        r.status = QFile::exists(r.localSource) ? FileStatus::Downloaded : FileStatus::Queued;
        return true;
    case FileStatus::Uploading:
        if (QFile::exists(r.localOutput)) {
            r.status = FileStatus::Encoded; // the replacement continues where HiDrive's state says
        } else {
            r.status = FileStatus::Failed;
            r.message = QCoreApplication::translate("tt::JobManager", "Interrupted while finishing and the local output is gone.");
        }
        return true;
    default: return false;
    }
}
} // namespace

QString formatBytes(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString formatMbps(qint64 bitsPerSecond)
{
    return QStringLiteral("%1 Mbit/s").arg(QLocale().toString(double(bitsPerSecond) / 1e6, 'f', 1));
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("–");
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 s = seconds % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

JobManager::JobManager(QObject *parent)
    : QObject(parent)
    , m_client(new HiDriveClient(this))
    , m_encoder(new Encoder(this))
{
    connect(m_encoder, &Encoder::progress, this, &JobManager::onEncodeProgress);
    connect(m_encoder, &Encoder::finished, this, &JobManager::onEncodeFinished);
    connect(&m_scanTimer, &QTimer::timeout, this, &JobManager::scanNow);
    connect(&m_tickTimer, &QTimer::timeout, this, &JobManager::schedule);
    connect(&m_purgeTimer, &QTimer::timeout, this, &JobManager::purgeBackups);
}

JobManager::~JobManager()
{
    shutdown();
}

Result<void> JobManager::initialize(const Settings &settings)
{
    m_settings = settings;
    for (const QString &dir : {m_settings.workDir, m_settings.backupDir, m_settings.logDir})
        QDir().mkpath(dir);
    if (auto ok = m_db.open(m_settings.dbPath); !ok)
        return ok;

    for (FileRecord &r : m_db.loadAll()) {
        if (recoverInterrupted(r)) // the app crashed or was killed
            m_db.save(r);
        m_records.emplace(r.path, r);
    }
    logInfo(tr("Database %1: %n file(s) known.", "", int(m_records.size())).arg(m_settings.dbPath));

    compileExcludes();
    configureClient();
    m_scanTimer.start(std::max(1, m_settings.pollMinutes) * 60000);
    m_tickTimer.start(5000);
    m_purgeTimer.start(3600 * 1000);
    QTimer::singleShot(0, this, &JobManager::purgeBackups);
    QTimer::singleShot(500, this, &JobManager::scanNow);
    return {};
}

void JobManager::applySettings(const Settings &settings)
{
    const QString oldLink = m_settings.resolvedShareLink();
    const QString oldPassword = m_settings.sharePassword;
    m_settings = settings;
    for (const QString &dir : {m_settings.workDir, m_settings.backupDir, m_settings.logDir})
        QDir().mkpath(dir);
    compileExcludes();
    configureClient();
    m_scanTimer.start(std::max(1, m_settings.pollMinutes) * 60000);

    for (auto &[path, r] : m_records) {
        if (r.probed && (r.status == FileStatus::SmallEnough || r.status == FileStatus::Queued)) {
            const FileStatus before = r.status;
            decideStatus(r);
            if (r.status != before)
                m_db.save(r);
        }
    }
    logInfo(tr("Settings applied (%1).").arg(m_settings.modifyRemote ? tr("replace originals on HiDrive")
                                                                     : tr("read-only mode")));
    emit recordsReset();
    emit stateChanged();
    schedule();
    if (m_settings.resolvedShareLink() != oldLink || m_settings.sharePassword != oldPassword)
        scanNow();
}

void JobManager::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_scanTimer.stop();
    m_tickTimer.stop();
    m_purgeTimer.stop();
    m_client->abortTransfers();
    m_encoder->abort(); // before the recovery below deletes its output
    for (auto &[path, r] : m_records) {
        if (recoverInterrupted(r))
            m_db.save(r);
    }
}

void JobManager::configureClient()
{
    QString error;
    const QString link = m_settings.resolvedShareLink(&error);
    Log::instance().addSecret(link);
    m_linkError.clear();
    std::optional<ShareLink> parsed;
    if (link.isEmpty())
        m_linkError = error.isEmpty() ? tr("No HiDrive share link configured.") : error;
    else if (parsed = parseShareLink(link); !parsed)
        m_linkError = tr("The share link is not a valid HiDrive /share/ link.");
    if (!m_linkError.isEmpty()) {
        logWarning(m_linkError);
        m_client->configure({}, {}, false);
        return;
    }
    // A running replacement is finished even if read-only mode was just switched on (see finalizeEnded).
    m_client->configure(*parsed, m_settings.sharePassword,
                        m_settings.modifyRemote || (!m_uploading.isEmpty() && m_client->writeAllowed()));
    logInfo(tr("Monitoring HiDrive folder %1 (%2).")
                .arg(parsed->basePath,
                     m_settings.modifyRemote ? tr("originals will be replaced") : tr("read-only")));
}

void JobManager::compileExcludes()
{
    m_excludes.clear();
    for (const QString &pattern : m_settings.excludes) {
        if (pattern.trimmed().isEmpty())
            continue;
        m_excludes.append(QRegularExpression(
            QRegularExpression::wildcardToRegularExpression(pattern.trimmed(), QRegularExpression::NonPathWildcardConversion),
            QRegularExpression::CaseInsensitiveOption));
    }
}

FileRecord *JobManager::find(const QString &path)
{
    const auto it = m_records.find(path);
    return it == m_records.end() ? nullptr : &it->second;
}

const FileRecord *JobManager::record(const QString &path) const
{
    const auto it = m_records.find(path);
    return it == m_records.end() ? nullptr : &it->second;
}

void JobManager::save(FileRecord &record, bool structural)
{
    m_db.save(record);
    if (structural)
        emit recordsReset();
    else
        emit recordUpdated(record.path);
}

void JobManager::throttledUpdate(const QString &path)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastUpdateEmit.value(path) < 400)
        return;
    m_lastUpdateEmit[path] = now;
    emit recordUpdated(path);
    emit stateChanged();
}

bool JobManager::isCandidate(const RemoteEntry &entry) const
{
    if (entry.isDir || entry.name.endsWith(QLatin1String(".tanztrans-part")))
        return false;
    const QString suffix = QFileInfo(entry.name).suffix().toLower();
    if (!m_settings.extensions.contains(suffix, Qt::CaseInsensitive))
        return false;
    const QString rel = relativeToBase(basePath(), entry.path);
    for (const QRegularExpression &re : m_excludes) {
        if (re.match(rel).hasMatch())
            return false;
    }
    return true;
}

bool JobManager::inEncodeWindow() const
{
    if (!m_settings.windowEnabled)
        return true;
    const QTime t = QTime::currentTime();
    const QTime a = m_settings.windowStart;
    const QTime b = m_settings.windowEnd;
    return a <= b ? (t >= a && t < b) : (t >= a || t < b);
}

QList<FileRecord *> JobManager::queueList()
{
    QList<FileRecord *> list;
    for (auto &[path, r] : m_records) {
        if (isQueueable(r.status))
            list.append(&r);
    }
    std::ranges::sort(list, [](const FileRecord *a, const FileRecord *b) { return queueBefore(*a, *b); });
    return list;
}

FileRecord *JobManager::nextWith(FileStatus status)
{
    const qint64 now = nowSecs();
    FileRecord *best = nullptr;
    for (auto &[path, r] : m_records) {
        if (r.status != status || r.notBefore > now || !isInsideBase(basePath(), r.path))
            continue;
        if (!best || queueBefore(r, *best))
            best = &r;
    }
    return best;
}

QStringList JobManager::orderedPaths(Filter filter) const
{
    QList<const FileRecord *> list;
    for (const auto &[path, r] : m_records) {
        bool include = true;
        switch (filter) {
        case Filter::All: break;
        case Filter::Active:
            include = isActive(r.status) || r.status == FileStatus::Queued || r.status == FileStatus::Downloaded
                      || r.status == FileStatus::Encoded || r.status == FileStatus::Pending;
            break;
        case Filter::Problems:
            include = r.status == FileStatus::Failed || r.status == FileStatus::NoGain || r.status == FileStatus::Missing;
            break;
        case Filter::Finished:
            include = r.status == FileStatus::Done || r.status == FileStatus::DoneLocal
                      || r.status == FileStatus::SmallEnough || r.status == FileStatus::AlreadyDone
                      || r.status == FileStatus::Skipped || r.status == FileStatus::Output
                      || r.status == FileStatus::NotVideo;
            break;
        }
        if (include)
            list.append(&r);
    }
    std::ranges::sort(list, [](const FileRecord *a, const FileRecord *b) {
        const int ra = statusRank(a->status);
        const int rb = statusRank(b->status);
        if (ra != rb)
            return ra < rb;
        if (ra >= 1 && ra <= 3)
            return queueBefore(*a, *b);
        if (a->mtime != b->mtime)
            return a->mtime > b->mtime;
        return a->path < b->path;
    });
    QStringList paths;
    for (const FileRecord *r : list)
        paths << r->path;
    return paths;
}

const FileRecord *JobManager::encodingRecord() const
{
    return m_encoding.isEmpty() ? nullptr : record(m_encoding);
}

bool JobManager::encoderSuspended() const
{
    return m_encoder->isSuspended();
}

qint64 JobManager::savedBytes() const
{
    qint64 saved = 0;
    for (const auto &[path, r] : m_records) {
        if ((r.status == FileStatus::Done || r.status == FileStatus::DoneLocal) && r.origSize > 0 && r.outSize > 0)
            saved += r.origSize - r.outSize;
    }
    return saved;
}

int JobManager::secondsToNextScan() const
{
    return m_scanTimer.isActive() ? m_scanTimer.remainingTime() / 1000 : -1;
}

bool JobManager::isIdle() const
{
    if (!hasScanned() || m_scanning || !m_probing.isEmpty() || !m_probeQueue.isEmpty() || !m_downloading.isEmpty()
        || !m_encoding.isEmpty() || !m_uploading.isEmpty())
        return false;
    for (const auto &[path, r] : m_records) {
        if (r.status == FileStatus::Pending || r.status == FileStatus::Queued || r.status == FileStatus::Downloaded
            || r.status == FileStatus::Encoded)
            return false;
    }
    return true;
}

bool JobManager::isCheckIdle() const
{
    return hasScanned() && !m_scanning && m_probing.isEmpty() && m_probeQueue.isEmpty();
}

void JobManager::setTools(const QString &ffmpeg, const QString &ffprobe)
{
    m_ffmpeg = ffmpeg;
    m_ffprobe = ffprobe;
    m_toolsReady = true;
    m_toolsError.clear();
    emit stateChanged();
    probeNext();
    schedule();
}

void JobManager::setToolsMissing(const QString &reason)
{
    m_toolsReady = false;
    m_toolsError = reason;
    emit stateChanged();
}

void JobManager::toolsLost(const QString &reason)
{
    const bool wasReady = m_toolsReady;
    setToolsMissing(reason);
    if (wasReady) {
        logError(tr("ffmpeg cannot be started any more: %1").arg(reason));
        emit toolsMissing();
    }
}

QString JobManager::statusText() const
{
    if (!m_linkError.isEmpty())
        return m_linkError;
    if (!m_toolsReady)
        return m_toolsError.isEmpty() ? tr("Looking for ffmpeg…")
                                      : tr("ffmpeg is missing – see Settings → Tools");
    if (m_state == RunState::Stopped)
        return tr("Stopped");
    if (m_state == RunState::Paused)
        return m_encoding.isEmpty() ? tr("Paused") : tr("Paused (encode suspended)");
    if (const FileRecord *r = encodingRecord()) {
        return r->progress >= 0 ? tr("Encoding %1 – %2 %").arg(r->name()).arg(int(r->progress * 100))
                                : tr("Encoding %1").arg(r->name());
    }
    if (!m_uploading.isEmpty())
        return tr("Uploading %1").arg(remoteName(m_uploading));
    if (!m_downloading.isEmpty())
        return tr("Downloading %1").arg(remoteName(m_downloading));
    if (m_scanning)
        return tr("Scanning HiDrive…");
    if (!m_probing.isEmpty())
        return tr("Checking %1").arg(remoteName(m_probing));
    if (!inEncodeWindow()) {
        return tr("Waiting for the encode window (%1–%2)")
            .arg(m_settings.windowStart.toString(QStringLiteral("HH:mm")),
                 m_settings.windowEnd.toString(QStringLiteral("HH:mm")));
    }
    return tr("Idle");
}

// ---------------------------------------------------------------- scanning

void JobManager::scanNow()
{
    if (m_scanning || m_shuttingDown)
        return;
    if (!m_client->isConfigured()) {
        logWarning(m_linkError.isEmpty() ? tr("No share link configured.") : m_linkError);
        return;
    }
    m_scanning = true;
    emit stateChanged();
    logDebug(tr("Scanning %1 …").arg(basePath()));
    m_client->listRecursive(basePath(), [this](Result<QList<RemoteEntry>> result) {
        if (!m_shuttingDown)
            onScanFinished(std::move(result));
    });
}

void JobManager::onScanFinished(Result<QList<RemoteEntry>> result)
{
    m_scanning = false;
    const qint64 now = nowSecs();
    if (!result) {
        logError(tr("Scan failed: %1").arg(result.error()));
        emit stateChanged();
        return;
    }
    const qint64 previousScan = m_lastScanTime;
    m_lastScanTime = now;

    QSet<QString> seen;
    QSet<QString> allFiles;
    int videos = 0;
    int added = 0;
    for (const RemoteEntry &e : *result) {
        allFiles.insert(e.path);
        if (!isCandidate(e))
            continue;
        ++videos;
        seen.insert(e.path);
        FileRecord *r = find(e.path);
        if (!r) {
            FileRecord n;
            n.path = e.path;
            n.remoteId = e.id;
            n.chash = e.chash;
            n.size = e.size;
            n.mtime = e.mtime;
            n.firstSeen = now;
            n.lastSeen = now;
            n.status = FileStatus::Pending;
            m_db.save(n);
            m_records.emplace(n.path, n);
            ++added;
            continue;
        }
        r->lastSeen = now;
        const bool changed = r->size != e.size || (!r->chash.isEmpty() && !e.chash.isEmpty() && r->chash != e.chash)
                             || (e.chash.isEmpty() && r->mtime != e.mtime);
        if (!changed && r->status != FileStatus::Missing)
            continue;
        if (isActive(r->status) || r->status == FileStatus::Encoded) {
            if (!m_recheck.contains(r->path))
                logWarning(tr("%1 changed on HiDrive while it is being processed; it will be checked again.").arg(r->name()));
            m_recheck.insert(r->path);
            continue;
        }
        if (r->status != FileStatus::Missing)
            logInfo(tr("%1 changed on HiDrive; checking it again.").arg(r->name()));
        cleanupLocal(*r);
        r->remoteId = e.id;
        r->chash = e.chash;
        r->size = e.size;
        r->mtime = e.mtime;
        r->firstSeen = now;
        r->retries = 0;
        r->notBefore = 0;
        r->probed = false;
        r->message.clear();
        if (r->status != FileStatus::Skipped)
            r->status = FileStatus::Pending;
        m_db.save(*r);
    }

    for (auto &[path, r] : m_records) {
        if (seen.contains(path) || !isInsideBase(basePath(), path) || !expectsRemote(r))
            continue;
        if (allFiles.contains(remoteParent(path) + QLatin1Char('/') + asideName(r.name()))) {
            // An interrupted same-name replacement: the original is still there under the aside name.
            logWarning(tr("%1 is in the middle of a replacement; it is finished on the next attempt.").arg(r.name()));
            continue;
        }
        cleanupLocal(r);
        r.status = FileStatus::Missing;
        r.message = tr("No longer on HiDrive.");
        m_db.save(r);
    }

    for (auto &[path, r] : m_records) {
        if (r.status != FileStatus::Pending || !seen.contains(path))
            continue;
        const bool stable = (previousScan > 0 && r.firstSeen <= previousScan)
                            || (now - r.mtime >= qint64(m_settings.stableMinutes) * 60);
        if (stable && !m_probeQueue.contains(path))
            m_probeQueue.append(path);
    }
    logInfo(tr("Scan finished: %n video(s) on HiDrive", "", videos)
            + (added > 0 ? tr(", %n new", "", added) : QString())
            + (m_probeQueue.isEmpty() ? QString() : tr(", %n to check", "", int(m_probeQueue.size()))));
    emit recordsReset();
    emit stateChanged();
    probeNext();
    schedule();
}

void JobManager::probeNext()
{
    while (m_toolsReady && m_probing.isEmpty() && !m_probeQueue.isEmpty() && !m_shuttingDown) {
        const QString path = m_probeQueue.takeFirst();
        FileRecord *r = find(path);
        if (!r || r->status != FileStatus::Pending)
            continue;
        m_probing = path;
        r->status = FileStatus::Probing;
        emit recordUpdated(path);
        emit stateChanged();
        m_client->ensureToken([this, path](Result<QString> token) {
            if (m_shuttingDown)
                return;
            if (!token) {
                ProbeResult failed;
                failed.error = token.error();
                finishProbe(path, failed);
                return;
            }
            runProbe(m_ffprobe, m_client->fileUrl(path), *token, this,
                     [this, path](ProbeResult probe) {
                         if (!m_shuttingDown)
                             finishProbe(path, probe);
                     });
        });
    }
}

void JobManager::finishProbe(const QString &path, const ProbeResult &p)
{
    m_probing.clear();
    if (p.startFailed) { // not the file's fault: put it back and wait for ffmpeg
        if (FileRecord *r = find(path)) {
            r->status = FileStatus::Pending;
            emit recordUpdated(path);
        }
        m_probeQueue.prepend(path);
        toolsLost(p.error);
        return;
    }
    if (FileRecord *r = find(path)) {
        if (!p.ok()) {
            r->retries++;
            r->message = p.error;
            if (r->retries >= 3) {
                r->status = FileStatus::Failed;
                logError(tr("Cannot check %1: %2").arg(r->name(), p.error));
                emit notify(tr("TanzTrans"), tr("Cannot check %1").arg(r->name()), true);
            } else {
                r->status = FileStatus::Pending;
                logWarning(tr("Cannot check %1 (will retry): %2").arg(r->name(), p.error));
            }
        } else {
            r->probed = true;
            r->hasVideo = p.hasVideo;
            r->tagged = p.taggedByUs();
            r->vcodec = p.vcodec;
            r->width = p.width;
            r->height = p.height;
            r->fps = p.fps;
            r->hdr = p.hdrLabel();
            r->vbitrate = p.videoBitrate;
            r->duration = p.duration;
            r->origSize = r->size;
            r->retries = 0;
            r->message.clear();
            decideStatus(*r);
            if (r->hasVideo) {
                logInfo(tr("%1: %2×%3, %4 fps%5, %6 (%7 limit %8) → %9")
                            .arg(r->name())
                            .arg(r->width)
                            .arg(r->height)
                            .arg(QLocale().toString(r->fps, 'f', 2))
                            .arg(r->hdr.isEmpty() ? QString() : QLatin1Char(' ') + r->hdr)
                            .arg(formatMbps(r->vbitrate), r->vcodec, formatMbps(r->thresholdBps),
                                 statusDisplayName(r->status)));
            } else {
                logInfo(tr("%1: no video stream.").arg(r->name()));
            }
        }
        m_db.save(*r);
        emit recordsReset();
    }
    emit stateChanged();
    probeNext();
    schedule();
}

void JobManager::decideStatus(FileRecord &r)
{
    const Decision d = decide(std::min(r.width, r.height), r.fps, r.vbitrate, r.hasVideo, r.tagged, m_settings);
    r.thresholdBps = d.thresholdBps;
    r.capBps = d.capBps;
    switch (d.kind) {
    case Decision::Kind::Transcode: r.status = FileStatus::Queued; break;
    case Decision::Kind::SmallEnough: r.status = r.force ? FileStatus::Queued : FileStatus::SmallEnough; break;
    case Decision::Kind::AlreadyOurs: r.status = FileStatus::AlreadyDone; break;
    case Decision::Kind::NotVideo: r.status = FileStatus::NotVideo; break;
    }
}

// ---------------------------------------------------------------- scheduling

void JobManager::schedule()
{
    if (m_shuttingDown || m_state != RunState::Running || !m_client->isConfigured())
        return;
    if (m_toolsReady && m_encoding.isEmpty() && inEncodeWindow()) {
        if (FileRecord *r = nextWith(FileStatus::Downloaded))
            startEncode(*r);
    }
    if (m_uploading.isEmpty()) {
        if (FileRecord *r = nextWith(FileStatus::Encoded))
            startFinalize(*r);
    }
    if (m_downloading.isEmpty()) {
        int waiting = 0;
        for (const auto &[path, r] : m_records) // same scope as nextWith(): inside the monitored folder
            waiting += r.status == FileStatus::Downloaded && isInsideBase(basePath(), path) ? 1 : 0;
        if (waiting == 0) {
            if (FileRecord *r = nextWith(FileStatus::Queued))
                startDownload(*r);
        }
    }
}

void JobManager::cleanupLocal(FileRecord &r)
{
    for (QString *file : {&r.localSource, &r.localOutput}) {
        if (!file->isEmpty()) {
            QFile::remove(*file);
            QFile::remove(*file + QStringLiteral(".part"));
            file->clear();
        }
    }
    r.progress = -1;
}

bool JobManager::consumeRecheck(FileRecord &r)
{
    if (!m_recheck.remove(r.path))
        return false;
    cleanupLocal(r);
    r.status = FileStatus::Pending;
    r.firstSeen = nowSecs();
    r.retries = 0;
    r.message = tr("Changed on HiDrive during processing.");
    save(r, true);
    return true;
}

void JobManager::failTransient(FileRecord &r, const QString &message, FileStatus retryStatus)
{
    r.retries++;
    r.message = message;
    r.progress = -1;
    if (r.retries <= 2) {
        const int delay = 60 * (1 << (r.retries - 1));
        r.status = retryStatus;
        r.notBefore = nowSecs() + delay;
        logWarning(tr("%1: %2 – retrying in %n minute(s).", "", delay / 60).arg(r.name(), message));
    } else {
        r.status = FileStatus::Failed;
        logError(tr("%1: %2").arg(r.name(), message));
        emit notify(tr("TanzTrans"), tr("%1 failed: %2").arg(r.name(), message), true);
    }
    save(r, true);
}

void JobManager::failPermanent(FileRecord &r, const QString &message)
{
    r.status = FileStatus::Failed;
    r.message = message;
    r.progress = -1;
    logError(tr("%1: %2").arg(r.name(), message));
    emit notify(tr("TanzTrans"), tr("%1 failed: %2").arg(r.name(), message), true);
    save(r, true);
}

void JobManager::jobCompleted()
{
    ++m_completedSinceIdle;
    bool more = false;
    for (const auto &[path, r] : m_records) {
        if (r.status == FileStatus::Queued || r.status == FileStatus::Downloaded || r.status == FileStatus::Encoded
            || isActive(r.status))
            more = true;
    }
    if (!more && m_completedSinceIdle > 0) {
        emit notify(tr("TanzTrans"), tr("Queue finished: %n file(s) processed.", "", m_completedSinceIdle), false);
        m_completedSinceIdle = 0;
    }
}

// ---------------------------------------------------------------- download

void JobManager::startDownload(FileRecord &r)
{
    QDir().mkpath(m_settings.workDir);
    const QStorageInfo storage(m_settings.workDir);
    const qint64 needed = r.size * 2 + 2LL * 1024 * 1024 * 1024;
    if (storage.isValid() && storage.bytesAvailable() < needed) {
        if (!m_diskWarned)
            logWarning(tr("Not enough free space in %1 for %2 (need %3).")
                           .arg(m_settings.workDir, r.name(), formatBytes(needed)));
        m_diskWarned = true;
        r.notBefore = nowSecs() + 600;
        return;
    }
    m_diskWarned = false;
    m_downloading = r.path;
    const QString suffix = QFileInfo(r.name()).suffix().toLower();
    r.localSource = QStringLiteral("%1/%2-source.%3").arg(m_settings.workDir).arg(r.id).arg(suffix);
    r.status = FileStatus::Downloading;
    r.progress = 0;
    save(r, true);
    emit stateChanged();
    logInfo(tr("Downloading %1 (%2).").arg(r.name(), formatBytes(r.size)));
    const QString path = r.path;
    m_client->download(
        path, r.localSource,
        [this, path](qint64 done, qint64 total) {
            if (FileRecord *rec = find(path); rec && total > 0) {
                rec->progress = double(done) / double(total);
                throttledUpdate(path);
            }
        },
        [this, path](Result<void> result) {
            if (!m_shuttingDown)
                onDownloadFinished(path, result);
        });
}

void JobManager::onDownloadFinished(const QString &path, const Result<void> &result)
{
    m_downloading.clear();
    FileRecord *r = find(path);
    if (!r)
        return;
    r->progress = -1;
    if (consumeRecheck(*r)) {
        // reset to pending
    } else if (!result) {
        if (result.error() == QLatin1String("aborted")) {
            r->status = FileStatus::Queued;
            save(*r, true);
        } else {
            failTransient(*r, tr("Download failed: %1").arg(result.error()), FileStatus::Queued);
        }
    } else if (QFileInfo(r->localSource).size() != r->size) {
        QFile::remove(r->localSource);
        failTransient(*r, tr("Downloaded size does not match HiDrive."), FileStatus::Queued);
    } else {
        r->status = FileStatus::Downloaded;
        r->retries = 0;
        r->notBefore = 0;
        r->message.clear();
        logInfo(tr("Downloaded %1.").arg(r->name()));
        save(*r, true);
    }
    emit stateChanged();
    schedule();
}

// ---------------------------------------------------------------- encode

void JobManager::startEncode(FileRecord &r)
{
    if (!QFile::exists(r.localSource)) {
        r.status = FileStatus::Queued;
        save(r, true);
        return;
    }
    m_encoding = r.path;
    r.status = FileStatus::Encoding;
    r.progress = 0;
    r.etaSeconds = -1;
    save(r, true);
    emit stateChanged();
    const QString path = r.path;
    runProbe(m_ffprobe, r.localSource, {}, this, [this, path](ProbeResult source) {
        if (!m_shuttingDown)
            encodeWithProbe(path, source);
    });
}

void JobManager::encodeWithProbe(const QString &path, const ProbeResult &src)
{
    FileRecord *r = find(path);
    if (!r || r->status != FileStatus::Encoding) { // stopped while probing
        m_encoding.clear();
        schedule();
        return;
    }
    if (src.startFailed || m_state == RunState::Stopped) {
        m_encoding.clear();
        r->status = FileStatus::Downloaded;
        save(*r, true);
        if (src.startFailed)
            toolsLost(src.error);
        emit stateChanged();
        return;
    }
    if (!src.ok() || !src.hasVideo) {
        m_encoding.clear();
        pauseIfRequested();
        cleanupLocal(*r);
        failPermanent(*r, tr("Cannot read the downloaded file: %1").arg(src.ok() ? tr("no video stream") : src.error));
        schedule();
        return;
    }
    m_encodeSource = src;
    const Decision d = decide(src, m_settings);
    if (d.capBps > 0)
        r->capBps = d.capBps;
    r->localOutput = QStringLiteral("%1/%2-output.mp4").arg(m_settings.workDir).arg(r->id);
    QFile::remove(r->localOutput);
    QString safeName = r->name();
    safeName.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9._-])")), QStringLiteral("_"));
    r->logPath = QStringLiteral("%1/jobs/%2-%3.log").arg(m_settings.logDir).arg(r->id).arg(safeName);
    const QStringList args = buildFfmpegArgs(src, r->capBps, m_settings, r->localSource, r->localOutput);

    logInfo(tr("Encoding %1: CRF %2, preset %3, max %4%5.")
                .arg(r->name())
                .arg(m_settings.crf)
                .arg(m_settings.preset, formatMbps(r->capBps),
                     src.dolbyVision ? tr(", Dolby Vision metadata dropped") : QString()));
    m_encodeTimer.start();
    if (!m_encoder->start(m_ffmpeg, args, r->logPath, src.duration, m_settings.lowPriority)) {
        m_encoding.clear();
        failPermanent(*r, tr("Encoder is busy."));
        return;
    }
    if (m_state == RunState::Paused)
        m_encoder->suspend();
    save(*r);
    emit stateChanged();
}

void JobManager::onEncodeProgress(double fraction, double fps, double speed)
{
    FileRecord *r = find(m_encoding);
    if (!r)
        return;
    r->progress = fraction;
    r->encodeFps = fps;
    if (speed > 0 && r->duration > 0)
        r->etaSeconds = qint64(r->duration * (1.0 - fraction) / speed);
    else if (fraction > 0.01)
        r->etaSeconds = qint64(double(m_encodeTimer.elapsed()) / 1000.0 * (1.0 - fraction) / fraction);
    throttledUpdate(r->path);
}

void JobManager::onEncodeFinished(bool ok, bool aborted, const QString &error)
{
    if (m_shuttingDown)
        return;
    const QString path = m_encoding;
    FileRecord *r = find(path);
    if (!r) {
        m_encoding.clear();
        schedule();
        return;
    }
    r->progress = -1;
    r->etaSeconds = -1;
    if (aborted) {
        m_encoding.clear();
        QFile::remove(r->localOutput);
        r->localOutput.clear();
        if (!consumeRecheck(*r)) {
            r->status = QFile::exists(r->localSource) ? FileStatus::Downloaded : FileStatus::Queued;
            r->message = tr("Stopped by user.");
            save(*r, true);
        }
        emit stateChanged();
        schedule();
        return;
    }
    if (consumeRecheck(*r)) {
        m_encoding.clear();
        emit stateChanged();
        schedule();
        return;
    }
    if (!ok && m_encoder->startFailed()) { // ffmpeg missing: not the file's fault
        m_encoding.clear();
        r->localOutput.clear();
        r->status = FileStatus::Downloaded;
        save(*r, true);
        toolsLost(error);
        return;
    }
    if (!ok) {
        m_encoding.clear();
        pauseIfRequested();
        QFile::remove(r->localOutput);
        r->localOutput.clear();
        r->retries++;
        r->message = error;
        if (r->retries <= 1) {
            r->status = FileStatus::Downloaded;
            r->notBefore = nowSecs() + 60;
            logWarning(tr("Encoding %1 failed, retrying once: %2").arg(r->name(), error));
            save(*r, true);
        } else {
            cleanupLocal(*r);
            failPermanent(*r, tr("Encoding failed: %1").arg(error));
        }
        emit stateChanged();
        schedule();
        return;
    }
    runProbe(m_ffprobe, r->localOutput, {}, this, [this, path](ProbeResult output) {
        if (!m_shuttingDown)
            onVerified(path, output);
    });
}

void JobManager::onVerified(const QString &path, const ProbeResult &out)
{
    m_encoding.clear();
    FileRecord *r = find(path);
    if (!r) {
        schedule();
        return;
    }
    if (out.startFailed) { // ffprobe vanished: encode again once the tools are back
        QFile::remove(r->localOutput);
        r->localOutput.clear();
        r->status = FileStatus::Downloaded;
        save(*r, true);
        toolsLost(out.error);
        return;
    }
    const qint64 srcSize = QFileInfo(r->localSource).size();
    const qint64 outSize = QFileInfo(r->localOutput).size();
    const VerifyResult v = verifyOutput(m_encodeSource, out, srcSize, outSize, m_settings.minGainPercent);
    r->outSize = outSize;
    r->outVbitrate = out.videoBitrate;
    const QString summary = tr("%1 → %2 (%3), video %4 → %5")
                                .arg(formatBytes(srcSize), formatBytes(outSize),
                                     srcSize > 0 ? QStringLiteral("−%1 %").arg(int(100.0 * (1.0 - double(outSize) / double(srcSize))))
                                                 : QString(),
                                     formatMbps(m_encodeSource.videoBitrate), formatMbps(out.videoBitrate));
    appendToLog(r->logPath, QStringLiteral("# verify: %1 %2").arg(v.ok ? QStringLiteral("ok") : v.message, summary));

    if (v.ok) {
        setLocalMtime(r->localOutput, r->mtime);
        r->status = FileStatus::Encoded;
        r->retries = 0;
        r->notBefore = 0;
        r->message.clear();
        logInfo(tr("Encoded %1 in %2: %3.").arg(r->name(), formatDuration(m_encodeTimer.elapsed() / 1000), summary));
        save(*r, true);
    } else if (v.noGain) {
        cleanupLocal(*r);
        r->status = FileStatus::NoGain;
        r->message = v.message;
        logInfo(tr("%1: %2 – original kept.").arg(r->name(), v.message));
        save(*r, true);
        jobCompleted();
    } else {
        cleanupLocal(*r);
        failPermanent(*r, tr("Verification failed: %1").arg(v.message));
    }
    pauseIfRequested();
    emit stateChanged();
    schedule();
}

void JobManager::pauseIfRequested()
{
    if (m_pauseAfterCurrent) {
        m_pauseAfterCurrent = false;
        m_state = RunState::Paused;
        logInfo(tr("Paused after the current job."));
    }
}

// ---------------------------------------------------------------- finalize
//
// Replace mode, in this order, so the original is never deleted before the new file is in place:
//   1. upload <name>.tanztrans-part                  (skipped if already there)
//   2. move the local original into the backup folder
//   3. pick a free final name: <stem>.mp4, else <stem>_tt.mp4; both taken -> nothing is changed
//   4. same name (an .mp4): rename the original to <name>.tanztrans-orig, then the upload to
//      <name> (the original is renamed back if that fails); new name: rename the upload
//   5. set the date, delete the original (or the .tanztrans-orig copy) if it is unchanged
// Every retry starts at 1 and works out from HiDrive's state how far an earlier attempt got.

void JobManager::startFinalize(FileRecord &r)
{
    if (consumeRecheck(r))
        return;
    if (!QFile::exists(r.localOutput)) {
        failPermanent(r, tr("The local output file is missing."));
        return;
    }
    if (!m_settings.modifyRemote) {
        finalizeLocal(r);
        return;
    }
    m_uploading = r.path;
    r.status = FileStatus::Uploading;
    r.progress = 0;
    save(r, true);
    emit stateChanged();
    finalizeUpload(r.path);
}

void JobManager::finalizeLocal(FileRecord &r)
{
    const QString dest = uniqueLocalPath(localMirrorPath(m_settings.outputDir, outputRelativePath(basePath(), r.path)));
    if (dest.isEmpty()) {
        failPermanent(r, tr("Cannot build a local file name for %1.").arg(r.path));
        return;
    }
    m_uploading = r.path; // one finalize at a time, also for local moves
    r.status = FileStatus::Uploading;
    save(r, true);
    emit stateChanged();
    const QString path = r.path;
    moveFileAsync(r.localOutput, dest, this, [this, path, dest](const QString &error) {
        m_uploading.clear();
        FileRecord *rec = find(path);
        if (!rec)
            return;
        if (!error.isEmpty()) {
            failPermanent(*rec, tr("Cannot move the output to %1: %2").arg(QDir::toNativeSeparators(dest), error));
            schedule();
            return;
        }
        setLocalMtime(dest, rec->mtime);
        QFile::remove(rec->localSource);
        rec->localSource.clear();
        rec->localOutput.clear();
        rec->outPath = dest;
        rec->status = FileStatus::DoneLocal;
        rec->message.clear();
        logInfo(tr("Read-only mode: saved %1 to %2.").arg(rec->name(), QDir::toNativeSeparators(dest)));
        save(*rec, true);
        emit stateChanged();
        jobCompleted();
        schedule();
    });
}

void JobManager::finalizeUpload(const QString &path)
{
    FileRecord *r = find(path);
    if (!r)
        return;
    const QString temp = remoteParent(path) + QLatin1Char('/') + tempName(r->name());
    const qint64 outSize = QFileInfo(r->localOutput).size();
    const QStringList finalNames = finalNameCandidates(r->name());
    m_client->meta(temp, [=, this](Result<std::optional<RemoteEntry>> existing) {
        if (m_shuttingDown)
            return;
        if (!existing) {
            finalizeFailed(path, existing.error());
            return;
        }
        if (existing->has_value() && (*existing)->size == outSize) {
            logInfo(tr("%1 is already uploaded.").arg(remoteName(temp)));
            finalizeBackup(path);
            return;
        }
        if (existing->has_value()) { // an interrupted upload: start over
            finalizeStartUpload(path);
            return;
        }
        // Nothing uploaded – unless an earlier attempt already put the new file in place.
        finalizeFindPlaced(path, finalNames, outSize, [this, path](const QString &placedName) {
            if (placedName.isEmpty()) {
                finalizeStartUpload(path);
            } else {
                logInfo(tr("%1 is already in place on HiDrive; finishing the replacement.").arg(placedName));
                finalizeCleanup(path, placedName);
            }
        });
    });
}

void JobManager::finalizeFindPlaced(const QString &path, QStringList names, qint64 outSize,
                                    std::function<void(const QString &)> done)
{
    if (names.isEmpty()) {
        done({});
        return;
    }
    const QString name = names.takeFirst();
    m_client->meta(remoteParent(path) + QLatin1Char('/') + name,
                   [=, this](Result<std::optional<RemoteEntry>> entry) {
                       if (m_shuttingDown)
                           return;
                       if (!entry) {
                           finalizeFailed(path, entry.error());
                           return;
                       }
                       if (entry->has_value() && (*entry)->size == outSize)
                           done(name);
                       else
                           finalizeFindPlaced(path, names, outSize, done);
                   });
}

void JobManager::finalizeStartUpload(const QString &path)
{
    FileRecord *r = find(path);
    if (!r)
        return;
    const QString tempFile = tempName(r->name());
    logInfo(tr("Uploading %1 (%2).").arg(r->name(), formatBytes(QFileInfo(r->localOutput).size())));
    m_client->upload(
        r->localOutput, remoteParent(path), tempFile,
        [this, path](qint64 done, qint64 total) {
            if (FileRecord *rec = find(path); rec && total > 0) {
                rec->progress = double(done) / double(total);
                throttledUpdate(path);
            }
        },
        [this, path](Result<RemoteEntry> uploaded) {
            if (m_shuttingDown)
                return;
            if (!uploaded)
                finalizeFailed(path, uploaded.error());
            else
                finalizeBackup(path);
        });
}

void JobManager::finalizeBackup(const QString &path)
{
    FileRecord *r = find(path);
    if (!r)
        return;
    // Already backed up by an earlier attempt: the work copy is gone and the backup has the
    // original's size. A backup of an earlier version of this path does not count.
    const qint64 originalSize = r->origSize > 0 ? r->origSize : r->size;
    const bool haveBackup = !r->backupPath.isEmpty() && QFile::exists(r->backupPath);
    if (haveBackup && !QFile::exists(r->localSource) && QFileInfo(r->backupPath).size() == originalSize) {
        finalizePickName(path);
        return;
    }
    if (!QFile::exists(r->localSource)) {
        finalizeFailed(path, tr("No local copy of the original – it will not be replaced on HiDrive."));
        return;
    }
    if (haveBackup) { // keep the earlier version's backup until its own expiry
        m_db.addOldBackup(r->backupPath, r->backupExpires);
        r->backupPath.clear();
        m_db.save(*r);
    }
    const QString dest = uniqueLocalPath(localMirrorPath(m_settings.backupDir, relativeToBase(basePath(), path)));
    if (dest.isEmpty()) {
        finalizeFailed(path, tr("Cannot build a backup file name for %1.").arg(path));
        return;
    }
    moveFileAsync(r->localSource, dest, this, [this, path, dest](const QString &error) {
        FileRecord *rec = find(path);
        if (!rec)
            return;
        if (error.isEmpty()) { // record the backup even while shutting down
            setLocalMtime(dest, rec->mtime);
            rec->backupPath = dest;
            rec->backupExpires = nowSecs() + qint64(m_settings.backupDays) * 86400;
            rec->localSource.clear();
            m_db.save(*rec);
        }
        if (m_shuttingDown)
            return;
        if (!error.isEmpty()) {
            finalizeFailed(path, tr("Cannot move the original to the backup folder %1: %2")
                                     .arg(QDir::toNativeSeparators(dest), error));
            return;
        }
        finalizePickName(path);
    });
}

void JobManager::finalizePickName(const QString &path)
{
    m_client->meta(path, [this, path](Result<std::optional<RemoteEntry>> original) {
        if (m_shuttingDown)
            return;
        FileRecord *r = find(path);
        if (!r)
            return;
        if (!original) {
            finalizeFailed(path, original.error());
            return;
        }
        if (original->has_value() && !unchangedOnRemote(*r, **original)) {
            logWarning(tr("%1 changed on HiDrive before it was replaced; the original is kept.").arg(r->name()));
            m_client->removeFile(remoteParent(path) + QLatin1Char('/') + tempName(r->name()), [](Result<void>) {});
            m_uploading.clear();
            m_recheck.insert(path);
            consumeRecheck(*r);
            finalizeEnded();
            return;
        }
        const QStringList names = finalNameCandidates(r->name());
        if (names.first() == r->name())
            finalizePlace(path, names.first());
        else
            finalizeFreeName(path, names);
    });
}

void JobManager::finalizeFreeName(const QString &path, QStringList names)
{
    if (names.isEmpty()) { // every candidate name is taken by another file: change nothing
        FileRecord *r = find(path);
        if (!r)
            return;
        const QStringList all = finalNameCandidates(r->name());
        m_client->removeFile(remoteParent(path) + QLatin1Char('/') + tempName(r->name()), [](Result<void>) {});
        m_uploading.clear();
        failPermanent(*r, tr("%1 and %2 already exist on HiDrive; nothing was changed.").arg(all.value(0), all.value(1)));
        finalizeEnded();
        return;
    }
    const QString name = names.takeFirst();
    m_client->meta(remoteParent(path) + QLatin1Char('/') + name, [=, this](Result<std::optional<RemoteEntry>> entry) {
        if (m_shuttingDown)
            return;
        if (!entry) {
            finalizeFailed(path, entry.error());
            return;
        }
        if (entry->has_value())
            finalizeFreeName(path, names);
        else
            finalizePlace(path, name);
    });
}

void JobManager::finalizePlace(const QString &path, const QString &finalName)
{
    FileRecord *r = find(path);
    if (!r)
        return;
    const QString name = r->name();
    const QString dir = remoteParent(path);
    const QString temp = dir + QLatin1Char('/') + tempName(name);

    const auto putNew = [=, this](bool restoreOriginal) {
        m_client->rename(temp, finalName, [=, this](Result<RemoteEntry> placed) {
            if (m_shuttingDown)
                return;
            if (placed) {
                finalizeCleanup(path, finalName);
                return;
            }
            const QString error = tr("Cannot rename the upload: %1").arg(placed.error());
            if (!restoreOriginal) {
                finalizeFailed(path, error);
                return;
            }
            m_client->rename(dir + QLatin1Char('/') + asideName(name), name, [=, this](Result<RemoteEntry>) {
                if (!m_shuttingDown)
                    finalizeFailed(path, error);
            });
        });
    };
    if (finalName != name) { // the original keeps its name until the new file is in place
        putNew(false);
        return;
    }
    // Same name: move the original aside first; it is deleted only after the new file is in place.
    m_client->meta(path, [=, this](Result<std::optional<RemoteEntry>> original) {
        if (m_shuttingDown)
            return;
        if (!original) {
            finalizeFailed(path, original.error());
            return;
        }
        if (!original->has_value()) { // already moved aside by an earlier attempt
            putNew(false);
            return;
        }
        m_client->rename(path, asideName(name), [=, this](Result<RemoteEntry> moved) {
            if (m_shuttingDown)
                return;
            if (!moved) {
                finalizeFailed(path, tr("Cannot rename the original: %1").arg(moved.error()));
                return;
            }
            putNew(true);
        });
    });
}

void JobManager::finalizeCleanup(const QString &path, const QString &finalName)
{
    FileRecord *r = find(path);
    if (!r)
        return;
    const QString name = r->name();
    const QString dir = remoteParent(path);
    const QString finalPath = dir + QLatin1Char('/') + finalName;
    // the original: the copy moved aside (same name) or the old file (new name)
    const QString original = finalName == name ? dir + QLatin1Char('/') + asideName(name) : path;

    const auto finish = [=, this] {
        m_client->meta(finalPath, [=, this](Result<std::optional<RemoteEntry>> current) {
            if (m_shuttingDown)
                return;
            if (!current) {
                finalizeFailed(path, current.error());
                return;
            }
            if (!current->has_value()) {
                finalizeFailed(path, tr("%1 is missing on HiDrive after the replacement.").arg(finalName));
                return;
            }
            finalizeDone(path, **current);
        });
    };
    m_client->setMtime(finalPath, r->mtime, [=, this](Result<void> set) {
        if (m_shuttingDown)
            return;
        if (!set)
            logWarning(tr("Cannot set the date of %1: %2").arg(finalName, set.error()));
        m_client->meta(original, [=, this](Result<std::optional<RemoteEntry>> entry) {
            if (m_shuttingDown)
                return;
            FileRecord *rec = find(path);
            if (!rec)
                return;
            if (!entry) {
                finalizeFailed(path, entry.error());
                return;
            }
            if (!entry->has_value()) {
                finish();
                return;
            }
            if (!unchangedOnRemote(*rec, **entry)) {
                logWarning(tr("%1 changed on HiDrive during the replacement; both files are kept.").arg(name));
                finish();
                return;
            }
            m_client->removeFile(original, [=, this](Result<void> removed) {
                if (m_shuttingDown)
                    return;
                if (!removed)
                    finalizeFailed(path, tr("Cannot delete the original: %1").arg(removed.error()));
                else
                    finish();
            });
        });
    });
}

void JobManager::finalizeDone(const QString &path, const RemoteEntry &entry)
{
    m_uploading.clear();
    FileRecord *r = find(path);
    if (!r) {
        finalizeEnded();
        return;
    }
    QFile::remove(r->localOutput);
    r->localOutput.clear();
    r->progress = -1;
    r->outPath = entry.path;
    r->outSize = entry.size;
    r->status = FileStatus::Done;
    r->retries = 0;
    r->message.clear();
    if (entry.path == r->path) {
        r->size = entry.size;
        r->chash = entry.chash;
    }
    const FileRecord snapshot = *r;
    m_db.save(*r);

    if (entry.path != path) {
        FileRecord *out = find(entry.path);
        if (!out) {
            FileRecord n;
            n.path = entry.path;
            m_records.emplace(n.path, n);
            out = find(entry.path);
        }
        out->remoteId = entry.id;
        out->chash = entry.chash;
        out->size = entry.size;
        out->mtime = snapshot.mtime;
        out->firstSeen = out->lastSeen = nowSecs();
        out->status = FileStatus::Output;
        out->probed = true;
        out->hasVideo = true;
        out->tagged = true;
        out->vcodec = QStringLiteral("hevc");
        out->width = snapshot.width;
        out->height = snapshot.height;
        out->fps = snapshot.fps;
        out->hdr = snapshot.hdr == QLatin1String("DV") ? QStringLiteral("HLG") : snapshot.hdr;
        out->vbitrate = snapshot.outVbitrate;
        out->duration = snapshot.duration;
        out->message = tr("Transcoded from %1").arg(snapshot.name());
        m_db.save(*out);
    }
    logInfo(tr("Replaced %1 on HiDrive with %2 (%3 → %4). Backup: %5")
                .arg(snapshot.name(), entry.name, formatBytes(snapshot.origSize), formatBytes(entry.size),
                     QDir::toNativeSeparators(snapshot.backupPath)));
    emit recordsReset();
    jobCompleted();
    finalizeEnded();
}

void JobManager::finalizeFailed(const QString &path, const QString &message)
{
    m_uploading.clear();
    if (FileRecord *r = find(path)) {
        r->progress = -1;
        if (message == kAbortedError) { // Stop: try again on Resume
            r->status = FileStatus::Encoded;
            save(*r, true);
        } else {
            failTransient(*r, message, FileStatus::Encoded);
        }
    }
    finalizeEnded();
}

void JobManager::finalizeEnded()
{
    // Write access is kept while a replacement runs, even if read-only mode was switched on meanwhile.
    if (m_client->writeAllowed() && !m_settings.modifyRemote)
        m_client->setWriteAllowed(false);
    emit stateChanged();
    schedule();
}

void JobManager::purgeBackups()
{
    const qint64 now = nowSecs();
    for (auto &[path, r] : m_records) {
        if (r.backupPath.isEmpty() || r.backupExpires > now)
            continue;
        // Keep the backup while a replacement may be unfinished or the file is gone on HiDrive.
        if (r.status == FileStatus::Failed || r.status == FileStatus::Encoded || r.status == FileStatus::Uploading
            || r.status == FileStatus::Missing)
            continue;
        if (QFile::remove(r.backupPath) || !QFile::exists(r.backupPath)) {
            logInfo(tr("Backup of %1 expired and was deleted.").arg(r.name()));
            r.backupPath.clear();
            m_db.save(r);
        }
    }
    for (const Database::OldBackup &old : m_db.oldBackups()) {
        if (old.expires > now)
            continue;
        if (QFile::remove(old.path) || !QFile::exists(old.path)) {
            logInfo(tr("Backup %1 expired and was deleted.").arg(QDir::toNativeSeparators(old.path)));
            m_db.removeOldBackup(old.path);
        }
    }
}

// ---------------------------------------------------------------- controls

void JobManager::pause()
{
    if (m_state != RunState::Running)
        return;
    m_state = RunState::Paused;
    if (m_encoder->isRunning())
        m_encoder->suspend();
    logInfo(tr("Paused."));
    emit stateChanged();
}

void JobManager::setPauseAfterCurrent(bool enabled)
{
    m_pauseAfterCurrent = enabled;
    logInfo(enabled ? tr("Will pause after the current job.") : tr("Pause after current job cancelled."));
    emit stateChanged();
}

void JobManager::stop()
{
    if (m_state == RunState::Stopped)
        return;
    m_state = RunState::Stopped;
    m_pauseAfterCurrent = false;
    logInfo(tr("Stopped: running jobs are aborted and stay in the queue."));
    m_client->abortTransfers();
    if (m_encoder->isRunning())
        m_encoder->abort();
    emit stateChanged();
}

void JobManager::resume()
{
    const RunState before = m_state;
    m_state = RunState::Running;
    if (m_encoder->isSuspended())
        m_encoder->resume();
    if (before != RunState::Running)
        logInfo(tr("Resumed."));
    emit stateChanged();
    schedule();
}

// ---------------------------------------------------------------- list actions

void JobManager::retry(const QString &path)
{
    FileRecord *r = find(path);
    if (!r || isActive(r->status))
        return;
    r->retries = 0;
    r->notBefore = 0;
    r->message.clear();
    if (QFile::exists(r->localOutput))
        r->status = FileStatus::Encoded;
    else if (QFile::exists(r->localSource))
        r->status = FileStatus::Downloaded;
    else if (r->probed && r->hasVideo)
        r->status = FileStatus::Queued;
    else {
        r->status = FileStatus::Pending;
        r->firstSeen = 0;
        if (!m_probeQueue.contains(path))
            m_probeQueue.append(path);
    }
    logInfo(tr("%1: retry requested.").arg(r->name()));
    save(*r, true);
    probeNext();
    schedule();
}

void JobManager::forceTranscode(const QString &path)
{
    FileRecord *r = find(path);
    if (!r || isActive(r->status) || !r->probed || !r->hasVideo)
        return;
    r->force = true;
    r->retries = 0;
    r->notBefore = 0;
    r->message.clear();
    r->status = FileStatus::Queued;
    logInfo(tr("%1: transcode forced.").arg(r->name()));
    save(*r, true);
    schedule();
}

int JobManager::localResultCount() const
{
    return int(std::ranges::count_if(m_records, [this](const auto &entry) {
        return entry.second.status == FileStatus::DoneLocal && isInsideBase(basePath(), entry.first);
    }));
}

void JobManager::requeueLocalResults()
{
    int count = 0;
    for (auto &[path, r] : m_records) {
        if (r.status != FileStatus::DoneLocal || !isInsideBase(basePath(), path))
            continue;
        r.status = FileStatus::Queued; // the local result stays in the output folder
        r.force = true;
        r.retries = 0;
        r.notBefore = 0;
        r.message = tr("Queued again to replace the original on HiDrive.");
        m_db.save(r);
        ++count;
    }
    logInfo(tr("%n file(s) from read-only mode queued again.", "", count));
    emit recordsReset();
    emit stateChanged();
    schedule();
}

void JobManager::skip(const QString &path)
{
    FileRecord *r = find(path);
    if (!r || isActive(r->status) || r->status == FileStatus::Encoded)
        return;
    cleanupLocal(*r);
    r->status = FileStatus::Skipped;
    r->force = false;
    logInfo(tr("%1 will not be transcoded.").arg(r->name()));
    save(*r, true);
}

void JobManager::unskip(const QString &path)
{
    FileRecord *r = find(path);
    if (!r || r->status != FileStatus::Skipped)
        return;
    if (r->probed)
        decideStatus(*r);
    else {
        r->status = FileStatus::Pending;
        r->firstSeen = 0;
        m_probeQueue.append(path);
    }
    save(*r, true);
    probeNext();
    schedule();
}

void JobManager::reorder(const QString &path, int delta, bool toTop)
{
    QList<FileRecord *> list = queueList();
    const qsizetype index = std::ranges::find_if(list, [&](const FileRecord *r) { return r->path == path; }) - list.begin();
    if (index >= list.size())
        return;
    const qsizetype to = toTop ? 0 : std::clamp<qsizetype>(index + delta, 0, list.size() - 1);
    if (to == index)
        return;
    list.move(index, to);
    for (qsizetype i = 0; i < list.size(); ++i) {
        const int priority = int(list.size() - i);
        if (list[i]->priority != priority) {
            list[i]->priority = priority;
            m_db.save(*list[i]);
        }
    }
    emit recordsReset();
}

void JobManager::moveToTop(const QString &path) { reorder(path, 0, true); }
void JobManager::moveUp(const QString &path) { reorder(path, -1, false); }
void JobManager::moveDown(const QString &path) { reorder(path, 1, false); }

} // namespace tt
