#pragma once

#include "Database.h"
#include "FileRecord.h"
#include "HiDriveClient.h"
#include "Probe.h"
#include "Settings.h"

#include <QElapsedTimer>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <map>

namespace tt {

class Encoder;

QString formatBytes(qint64 bytes);
QString formatMbps(qint64 bitsPerSecond);
QString formatDuration(qint64 seconds);

/// Orchestrates scanning, probing, downloading, encoding, verifying and replacing.
/// Everything runs on the GUI thread; the heavy lifting happens in ffmpeg/ffprobe
/// processes and asynchronous network replies.
class JobManager : public QObject
{
    Q_OBJECT
public:
    enum class RunState { Running, Paused, Stopped };
    enum class Filter { All, Active, Problems, Finished };

    explicit JobManager(QObject *parent = nullptr);
    ~JobManager() override;

    Result<void> initialize(const Settings &settings);
    void applySettings(const Settings &settings);
    [[nodiscard]] const Settings &settings() const { return m_settings; }
    void shutdown();

    [[nodiscard]] RunState runState() const { return m_state; }
    [[nodiscard]] bool pauseAfterCurrent() const { return m_pauseAfterCurrent; }
    [[nodiscard]] bool isScanning() const { return m_scanning; }
    [[nodiscard]] bool hasScanned() const { return m_lastScanTime > 0; }
    [[nodiscard]] bool isIdle() const;
    /// Scan and all probes finished (ignores the transcode queue).
    [[nodiscard]] bool isCheckIdle() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString linkError() const { return m_linkError; }
    [[nodiscard]] QString basePath() const { return m_client->link().basePath; }
    [[nodiscard]] int secondsToNextScan() const;
    [[nodiscard]] qint64 savedBytes() const;
    [[nodiscard]] const FileRecord *encodingRecord() const;
    [[nodiscard]] bool encoderSuspended() const;

    [[nodiscard]] const FileRecord *record(const QString &path) const;
    [[nodiscard]] QStringList orderedPaths(Filter filter) const;

    HiDriveClient *client() { return m_client; }

    /// ffmpeg/ffprobe to use (found by locateTools()). Until this is called, files are
    /// listed but not checked or encoded.
    void setTools(const QString &ffmpeg, const QString &ffprobe);
    /// No usable ffmpeg: checks and encodes wait; the reason shows in the status.
    void setToolsMissing(const QString &reason);
    [[nodiscard]] bool toolsReady() const { return m_toolsReady; }

    // Controls
    void scanNow();
    void pause();
    void setPauseAfterCurrent(bool enabled);
    void stop();
    void resume();

    // List actions
    void retry(const QString &path);
    void forceTranscode(const QString &path);
    void skip(const QString &path);
    void unskip(const QString &path);
    void moveToTop(const QString &path);
    void moveUp(const QString &path);
    void moveDown(const QString &path);

    /// Files finished in read-only mode (results only in the local output folder).
    [[nodiscard]] int localResultCount() const;
    /// Queues them again so that replace mode also replaces their originals on HiDrive.
    void requeueLocalResults();

signals:
    void recordsReset();
    void recordUpdated(const QString &path);
    void stateChanged();
    void notify(const QString &title, const QString &text, bool error);
    /// ffmpeg or ffprobe could not be started (deleted or moved while running).
    void toolsMissing();

private:
    void toolsLost(const QString &reason);
    FileRecord *find(const QString &path);
    void save(FileRecord &record, bool structural = false);
    void configureClient();
    void compileExcludes();
    [[nodiscard]] bool isCandidate(const RemoteEntry &entry) const;
    [[nodiscard]] bool inEncodeWindow() const;
    [[nodiscard]] QList<FileRecord *> queueList();
    FileRecord *nextWith(FileStatus status);
    void reorder(const QString &path, int delta, bool toTop);
    void cleanupLocal(FileRecord &record);
    bool consumeRecheck(FileRecord &record);
    void decideStatus(FileRecord &record);
    void failTransient(FileRecord &record, const QString &message, FileStatus retryStatus);
    void failPermanent(FileRecord &record, const QString &message);
    void jobCompleted();
    void pauseIfRequested();
    void throttledUpdate(const QString &path);

    void onScanFinished(Result<QList<RemoteEntry>> result);
    void probeNext();
    void finishProbe(const QString &path, const ProbeResult &probe);

    void schedule();
    void startDownload(FileRecord &record);
    void onDownloadFinished(const QString &path, const Result<void> &result);
    void startEncode(FileRecord &record);
    void encodeWithProbe(const QString &path, const ProbeResult &source);
    void onEncodeProgress(double fraction, double fps, double speed);
    void onEncodeFinished(bool ok, bool aborted, const QString &error);
    void onVerified(const QString &path, const ProbeResult &output);

    // Finishing a verified transcode; the replace order is described in JobManager.cpp.
    void startFinalize(FileRecord &record);
    void finalizeLocal(FileRecord &record);
    void finalizeUpload(const QString &path);
    void finalizeFindPlaced(const QString &path, QStringList names, qint64 outSize,
                            std::function<void(const QString &)> done);
    void finalizeStartUpload(const QString &path);
    void finalizeBackup(const QString &path);
    void finalizePickName(const QString &path);
    void finalizeFreeName(const QString &path, QStringList names);
    void finalizePlace(const QString &path, const QString &finalName);
    void finalizeCleanup(const QString &path, const QString &finalName);
    void finalizeDone(const QString &path, const RemoteEntry &entry);
    void finalizeFailed(const QString &path, const QString &message);
    void finalizeEnded();

    void purgeBackups();

    Settings m_settings;
    Database m_db;
    HiDriveClient *m_client = nullptr;
    Encoder *m_encoder = nullptr;
    std::map<QString, FileRecord> m_records;
    QList<QRegularExpression> m_excludes;
    QString m_linkError;
    QString m_ffmpeg;
    QString m_ffprobe;
    QString m_toolsError;
    bool m_toolsReady = false;

    RunState m_state = RunState::Running;
    bool m_pauseAfterCurrent = false;
    bool m_shuttingDown = false;
    bool m_scanning = false;
    bool m_diskWarned = false;
    qint64 m_lastScanTime = 0;
    QStringList m_probeQueue;
    QString m_probing;
    QString m_downloading;
    QString m_encoding;
    QString m_uploading;
    QSet<QString> m_recheck;
    ProbeResult m_encodeSource;
    QElapsedTimer m_encodeTimer;
    int m_completedSinceIdle = 0;
    QHash<QString, qint64> m_lastUpdateEmit;

    QTimer m_scanTimer;
    QTimer m_tickTimer;
    QTimer m_purgeTimer;
};

} // namespace tt
