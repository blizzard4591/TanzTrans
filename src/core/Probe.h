#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

namespace tt {

/// The parts of an ffprobe result TanzTrans cares about.
struct ProbeResult
{
    QString error;
    bool startFailed = false; ///< ffprobe itself could not be started (missing tools, not a file problem)

    bool hasVideo = false;
    int videoIndex = -1;
    QString vcodec;
    QString vprofile;
    QString vtag;
    QString pixFmt;
    int width = 0;
    int height = 0;
    double fps = 0;
    QString colorPrimaries;
    QString colorTransfer;
    QString colorSpace;
    bool dolbyVision = false;
    qint64 videoBitrate = 0; ///< bit/s

    int audioIndex = -1;     ///< stream to keep (first AAC, else first decodable audio)
    QString acodec;
    bool audioCopy = false;  ///< true if audioIndex is AAC and can be copied
    int audioStreams = 0;

    double duration = 0;     ///< seconds
    qint64 size = 0;
    qint64 formatBitrate = 0;
    QString comment;

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
    [[nodiscard]] bool taggedByUs() const { return comment.startsWith(QLatin1String("TanzTrans/")); }
    [[nodiscard]] int shortSide() const { return std::min(width, height); }
    /// "HLG", "HDR10", "DV" (Dolby Vision) or "" for SDR.
    [[nodiscard]] QString hdrLabel() const;
};

ProbeResult parseProbeJson(const QByteArray &json);

/// Runs ffprobe asynchronously. `input` is a local file or an https URL; a non-empty
/// bearer token is sent as Authorization header (never logged).
void runProbe(const QString &ffprobe, const QString &input, const QString &bearerToken, QObject *context,
              std::function<void(ProbeResult)> done);

} // namespace tt
