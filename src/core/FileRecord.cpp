#include "FileRecord.h"

#include <QCoreApplication>

#include <array>
#include <utility>

namespace tt {

namespace {
constexpr std::array kKeys{
    std::pair{FileStatus::Pending, "pending"},     std::pair{FileStatus::Probing, "probing"},
    std::pair{FileStatus::SmallEnough, "small"},   std::pair{FileStatus::AlreadyDone, "already-done"},
    std::pair{FileStatus::NotVideo, "not-video"},  std::pair{FileStatus::Queued, "queued"},
    std::pair{FileStatus::Downloading, "downloading"}, std::pair{FileStatus::Downloaded, "downloaded"},
    std::pair{FileStatus::Encoding, "encoding"},   std::pair{FileStatus::Encoded, "encoded"},
    std::pair{FileStatus::Uploading, "uploading"}, std::pair{FileStatus::Done, "done"},
    std::pair{FileStatus::DoneLocal, "done-local"}, std::pair{FileStatus::NoGain, "no-gain"},
    std::pair{FileStatus::Failed, "failed"},       std::pair{FileStatus::Skipped, "skipped"},
    std::pair{FileStatus::Missing, "missing"},     std::pair{FileStatus::Output, "output"},
};
} // namespace

QString statusKey(FileStatus status)
{
    for (const auto &[s, key] : kKeys) {
        if (s == status)
            return QString::fromLatin1(key);
    }
    return QStringLiteral("pending");
}

FileStatus statusFromKey(const QString &key)
{
    for (const auto &[s, k] : kKeys) {
        if (key == QLatin1String(k))
            return s;
    }
    return FileStatus::Pending;
}

QString statusDisplayName(FileStatus status)
{
    switch (status) {
    case FileStatus::Pending: return QCoreApplication::translate("Status", "Waiting until stable");
    case FileStatus::Probing: return QCoreApplication::translate("Status", "Checking");
    case FileStatus::SmallEnough: return QCoreApplication::translate("Status", "Small enough");
    case FileStatus::AlreadyDone: return QCoreApplication::translate("Status", "Already transcoded");
    case FileStatus::NotVideo: return QCoreApplication::translate("Status", "No video");
    case FileStatus::Queued: return QCoreApplication::translate("Status", "Queued");
    case FileStatus::Downloading: return QCoreApplication::translate("Status", "Downloading");
    case FileStatus::Downloaded: return QCoreApplication::translate("Status", "Ready to encode");
    case FileStatus::Encoding: return QCoreApplication::translate("Status", "Encoding");
    case FileStatus::Encoded: return QCoreApplication::translate("Status", "Encoded");
    case FileStatus::Uploading: return QCoreApplication::translate("Status", "Uploading");
    case FileStatus::Done: return QCoreApplication::translate("Status", "Replaced");
    case FileStatus::DoneLocal: return QCoreApplication::translate("Status", "Done (local)");
    case FileStatus::NoGain: return QCoreApplication::translate("Status", "No gain");
    case FileStatus::Failed: return QCoreApplication::translate("Status", "Failed");
    case FileStatus::Skipped: return QCoreApplication::translate("Status", "Skipped");
    case FileStatus::Missing: return QCoreApplication::translate("Status", "Missing");
    case FileStatus::Output: return QCoreApplication::translate("Status", "TanzTrans output");
    }
    return {};
}

bool isActive(FileStatus status)
{
    return status == FileStatus::Probing || status == FileStatus::Downloading || status == FileStatus::Encoding
           || status == FileStatus::Uploading;
}

bool isQueueable(FileStatus status)
{
    return status == FileStatus::Queued || status == FileStatus::Downloaded;
}

QString FileRecord::name() const
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

} // namespace tt
