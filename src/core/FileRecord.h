#pragma once

#include <QString>

namespace tt {

enum class FileStatus {
    Pending,     ///< seen, waiting until it is stable
    Probing,
    SmallEnough, ///< below the threshold, nothing to do
    AlreadyDone, ///< carries the TanzTrans tag
    NotVideo,
    Queued,
    Downloading,
    Downloaded,
    Encoding,
    Encoded,     ///< verified output waiting for upload / move
    Uploading,
    Done,        ///< original replaced on HiDrive
    DoneLocal,   ///< read-only mode: output in the local output folder
    NoGain,
    Failed,
    Skipped,     ///< user: never transcode
    Missing,     ///< no longer on HiDrive
    Output,      ///< a file TanzTrans uploaded
};

QString statusKey(FileStatus status);
FileStatus statusFromKey(const QString &key);
QString statusDisplayName(FileStatus status);

bool isActive(FileStatus status);    ///< a transfer/encode is running for it
bool isQueueable(FileStatus status); ///< Queued or Downloaded: position in the queue matters

struct FileRecord
{
    qint64 id = 0;
    QString path;            ///< remote path, decoded, absolute within the share
    QString remoteId;
    QString chash;
    qint64 size = 0;
    qint64 origSize = 0;     ///< size of the original when it was probed
    qint64 mtime = 0;        ///< unix seconds
    qint64 firstSeen = 0;    ///< when size/hash were last seen changing
    qint64 lastSeen = 0;
    FileStatus status = FileStatus::Pending;
    int priority = 0;
    int retries = 0;
    qint64 notBefore = 0;
    bool force = false;

    bool probed = false;
    bool hasVideo = false;
    bool tagged = false;
    QString vcodec;
    int width = 0;
    int height = 0;
    double fps = 0;
    QString hdr;
    qint64 vbitrate = 0;
    double duration = 0;
    qint64 thresholdBps = 0;
    qint64 capBps = 0;

    QString localSource;
    QString localOutput;
    QString outPath;
    qint64 outSize = 0;
    qint64 outVbitrate = 0;
    QString backupPath;
    qint64 backupExpires = 0;
    QString logPath;
    QString message;
    qint64 updated = 0;

    // transient, not stored
    double progress = -1;   ///< 0..1 while a transfer/encode runs
    double encodeFps = 0;
    qint64 etaSeconds = -1;

    [[nodiscard]] QString name() const;
};

} // namespace tt
