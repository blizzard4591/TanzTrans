#include "FfmpegArgs.h"

#include <QSet>

namespace tt {

namespace {

// Names accepted by both ffmpeg's color options and x265's VUI parameters.
const QSet<QString> &knownPrimaries()
{
    static const QSet<QString> v{QStringLiteral("bt709"),    QStringLiteral("bt470m"),   QStringLiteral("bt470bg"),
                                 QStringLiteral("smpte170m"), QStringLiteral("smpte240m"), QStringLiteral("film"),
                                 QStringLiteral("bt2020"),   QStringLiteral("smpte428"), QStringLiteral("smpte431"),
                                 QStringLiteral("smpte432")};
    return v;
}

const QSet<QString> &knownTransfers()
{
    static const QSet<QString> v{QStringLiteral("bt709"),        QStringLiteral("bt470m"),
                                 QStringLiteral("bt470bg"),      QStringLiteral("smpte170m"),
                                 QStringLiteral("smpte240m"),    QStringLiteral("linear"),
                                 QStringLiteral("iec61966-2-4"), QStringLiteral("bt1361e"),
                                 QStringLiteral("iec61966-2-1"), QStringLiteral("bt2020-10"),
                                 QStringLiteral("bt2020-12"),    QStringLiteral("smpte2084"),
                                 QStringLiteral("smpte428"),     QStringLiteral("arib-std-b67")};
    return v;
}

const QSet<QString> &knownMatrices()
{
    static const QSet<QString> v{QStringLiteral("bt709"),     QStringLiteral("fcc"),      QStringLiteral("bt470bg"),
                                 QStringLiteral("smpte170m"), QStringLiteral("smpte240m"), QStringLiteral("ycgco"),
                                 QStringLiteral("bt2020nc"),  QStringLiteral("bt2020c"),  QStringLiteral("smpte2085"),
                                 QStringLiteral("ictcp")};
    return v;
}

} // namespace

QString encoderTag(const Settings &s, qint64 capBps)
{
    return QStringLiteral("TanzTrans/1 x265 crf=%1 preset=%2 maxrate=%3k").arg(s.crf).arg(s.preset).arg(capBps / 1000);
}

QStringList buildFfmpegArgs(const ProbeResult &source, qint64 capBps, const Settings &s, const QString &input,
                            const QString &output)
{
    const qint64 capK = capBps / 1000;
    const qint64 bufK = qint64(double(capK) * s.bufsizeFactor);

    QStringList x265{QStringLiteral("vbv-maxrate=%1").arg(capK), QStringLiteral("vbv-bufsize=%1").arg(bufK),
                     QStringLiteral("log-level=error")};
    QStringList color;
    if (knownPrimaries().contains(source.colorPrimaries)) {
        x265 << QStringLiteral("colorprim=%1").arg(source.colorPrimaries);
        color << QStringLiteral("-color_primaries") << source.colorPrimaries;
    }
    if (knownTransfers().contains(source.colorTransfer)) {
        x265 << QStringLiteral("transfer=%1").arg(source.colorTransfer);
        color << QStringLiteral("-color_trc") << source.colorTransfer;
    }
    if (knownMatrices().contains(source.colorSpace)) {
        x265 << QStringLiteral("colormatrix=%1").arg(source.colorSpace);
        color << QStringLiteral("-colorspace") << source.colorSpace;
    }

    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
                     QStringLiteral("-loglevel"),    QStringLiteral("warning"), QStringLiteral("-i"),
                     input,
                     QStringLiteral("-map"),         QStringLiteral("0:%1").arg(source.videoIndex)};
    if (source.audioIndex >= 0)
        args << QStringLiteral("-map") << QStringLiteral("0:%1").arg(source.audioIndex);

    args << QStringLiteral("-c:v") << QStringLiteral("libx265")
         << QStringLiteral("-preset") << s.preset
         << QStringLiteral("-crf") << QString::number(s.crf)
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p10le")
         << QStringLiteral("-profile:v") << QStringLiteral("main10")
         << QStringLiteral("-dolbyvision") << QStringLiteral("0") // keep the HLG base only (ffmpeg >= 7.1 defaults to auto)
         << QStringLiteral("-x265-params") << x265.join(QLatin1Char(':'))
         << QStringLiteral("-tag:v") << QStringLiteral("hvc1")
         << QStringLiteral("-fps_mode") << QStringLiteral("passthrough");
    args << color;

    if (source.audioIndex < 0)
        args << QStringLiteral("-an");
    else if (source.audioCopy)
        args << QStringLiteral("-c:a") << QStringLiteral("copy");
    else
        args << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a") << QStringLiteral("192k");

    // Keep date, GPS, make/model (incl. com.apple.quicktime.* keys) but not the source's container
    // brand tags, which use_metadata_tags would otherwise copy into the MP4 as metadata keys.
    args << QStringLiteral("-map_metadata") << QStringLiteral("0")
         << QStringLiteral("-metadata") << QStringLiteral("major_brand=")
         << QStringLiteral("-metadata") << QStringLiteral("minor_version=")
         << QStringLiteral("-metadata") << QStringLiteral("compatible_brands=")
         << QStringLiteral("-movflags") << QStringLiteral("+faststart+use_metadata_tags")
         << QStringLiteral("-metadata") << QStringLiteral("comment=%1").arg(encoderTag(s, capBps))
         << QStringLiteral("-max_muxing_queue_size") << QStringLiteral("4096")
         << QStringLiteral("-progress") << QStringLiteral("pipe:1")
         << QStringLiteral("-nostats")
         << output;
    return args;
}

} // namespace tt
