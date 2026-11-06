#include "Probe.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <memory>

namespace tt {

namespace {

double parseRate(const QString &rate)
{
    const auto parts = rate.split(QLatin1Char('/'));
    if (parts.size() != 2)
        return rate.toDouble();
    const double num = parts[0].toDouble();
    const double den = parts[1].toDouble();
    return den > 0 ? num / den : 0;
}

qint64 toInt64(const QJsonValue &value)
{
    return value.isString() ? value.toString().toLongLong() : qint64(value.toDouble());
}

double toDouble(const QJsonValue &value)
{
    return value.isString() ? value.toString().toDouble() : value.toDouble();
}

QString tagValue(const QJsonObject &tags, QLatin1StringView key)
{
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        if (it.key().compare(key, Qt::CaseInsensitive) == 0)
            return it.value().toString();
    }
    return {};
}

bool isUnset(const QString &value)
{
    return value.isEmpty() || value == QLatin1String("unknown") || value == QLatin1String("reserved")
           || value == QLatin1String("unspecified");
}

} // namespace

QString ProbeResult::hdrLabel() const
{
    if (dolbyVision)
        return QStringLiteral("DV");
    if (colorTransfer == QLatin1String("arib-std-b67"))
        return QStringLiteral("HLG");
    if (colorTransfer == QLatin1String("smpte2084"))
        return QStringLiteral("HDR10");
    return {};
}

ProbeResult parseProbeJson(const QByteArray &json)
{
    ProbeResult r;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("invalid ffprobe output: %1").arg(parseError.errorString());
        return r;
    }
    const QJsonObject root = doc.object();
    const QJsonObject format = root.value(QLatin1String("format")).toObject();
    if (format.isEmpty()) {
        r.error = QStringLiteral("ffprobe returned no format information");
        return r;
    }
    r.duration = toDouble(format.value(QLatin1String("duration")));
    r.size = toInt64(format.value(QLatin1String("size")));
    r.formatBitrate = toInt64(format.value(QLatin1String("bit_rate")));
    r.comment = tagValue(format.value(QLatin1String("tags")).toObject(), QLatin1StringView("comment"));

    qint64 audioBitrateSum = 0;
    const QJsonArray streams = root.value(QLatin1String("streams")).toArray();
    for (const QJsonValue &value : streams) {
        const QJsonObject s = value.toObject();
        const QString type = s.value(QLatin1String("codec_type")).toString();
        const int index = s.value(QLatin1String("index")).toInt();
        const QString codec = s.value(QLatin1String("codec_name")).toString();

        if (type == QLatin1String("video")) {
            if (s.value(QLatin1String("disposition")).toObject().value(QLatin1String("attached_pic")).toInt() == 1)
                continue;
            if (r.hasVideo)
                continue;
            r.hasVideo = true;
            r.videoIndex = index;
            r.vcodec = codec;
            r.vprofile = s.value(QLatin1String("profile")).toString();
            r.vtag = s.value(QLatin1String("codec_tag_string")).toString();
            r.pixFmt = s.value(QLatin1String("pix_fmt")).toString();
            r.width = s.value(QLatin1String("width")).toInt();
            r.height = s.value(QLatin1String("height")).toInt();
            r.fps = parseRate(s.value(QLatin1String("avg_frame_rate")).toString());
            if (r.fps <= 0 || r.fps > 1000)
                r.fps = parseRate(s.value(QLatin1String("r_frame_rate")).toString());
            const auto color = [&s](QLatin1StringView key) {
                const QString v = s.value(key).toString();
                return isUnset(v) ? QString() : v;
            };
            r.colorPrimaries = color(QLatin1StringView("color_primaries"));
            r.colorTransfer = color(QLatin1StringView("color_transfer"));
            r.colorSpace = color(QLatin1StringView("color_space"));
            r.videoBitrate = toInt64(s.value(QLatin1String("bit_rate")));
            if (r.videoBitrate <= 0) // Matroska stores it as a tag
                r.videoBitrate = tagValue(s.value(QLatin1String("tags")).toObject(), QLatin1StringView("BPS")).toLongLong();
            for (const QJsonValue &sd : s.value(QLatin1String("side_data_list")).toArray()) {
                const QString sdType = sd.toObject().value(QLatin1String("side_data_type")).toString();
                if (sdType.contains(QLatin1String("DOVI"), Qt::CaseInsensitive)
                    || sdType.contains(QLatin1String("Dolby Vision"), Qt::CaseInsensitive))
                    r.dolbyVision = true;
            }
        } else if (type == QLatin1String("audio")) {
            ++r.audioStreams;
            audioBitrateSum += toInt64(s.value(QLatin1String("bit_rate")));
            const bool decodable = !isUnset(codec) && codec != QLatin1String("none");
            if (codec == QLatin1String("aac") && !(r.audioIndex >= 0 && r.audioCopy)) {
                r.audioIndex = index;
                r.acodec = codec;
                r.audioCopy = true;
            } else if (decodable && r.audioIndex < 0) {
                r.audioIndex = index;
                r.acodec = codec;
                r.audioCopy = false;
            }
        }
    }

    if (r.hasVideo && r.videoBitrate <= 0) {
        if (r.formatBitrate > 0)
            r.videoBitrate = std::max<qint64>(0, r.formatBitrate - audioBitrateSum);
        else if (r.duration > 0 && r.size > 0)
            r.videoBitrate = std::max<qint64>(0, qint64(double(r.size) * 8.0 / r.duration) - audioBitrateSum);
    }
    return r;
}

void runProbe(const QString &ffprobe, const QString &input, const QString &bearerToken, QObject *context,
              std::function<void(ProbeResult)> done)
{
    auto *process = new QProcess(context);
    auto finished = std::make_shared<bool>(false);
    auto complete = [process, finished, done = std::move(done)](ProbeResult result) {
        if (*finished)
            return;
        *finished = true;
        process->deleteLater();
        done(std::move(result));
    };

    QStringList args{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-print_format"),
                     QStringLiteral("json"), QStringLiteral("-show_format"), QStringLiteral("-show_streams")};
    if (!bearerToken.isEmpty()) {
        args << QStringLiteral("-headers")
             << QStringLiteral("Authorization: Bearer %1\r\n").arg(bearerToken);
        args << QStringLiteral("-rw_timeout") << QStringLiteral("60000000");
    }
    args << input;

    QObject::connect(process, &QProcess::finished, process, [process, complete](int code, QProcess::ExitStatus status) {
        const QByteArray out = process->readAllStandardOutput();
        if (status != QProcess::NormalExit || code != 0) {
            ProbeResult r;
            QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
            r.error = QStringLiteral("ffprobe failed (exit %1): %2").arg(code).arg(err.right(500));
            complete(r);
            return;
        }
        complete(parseProbeJson(out));
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            ProbeResult r;
            r.error = QStringLiteral("cannot start ffprobe: %1").arg(process->errorString());
            r.startFailed = true;
            complete(r);
        }
    });
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, process, [process, complete] {
        process->kill();
        ProbeResult r;
        r.error = QStringLiteral("ffprobe timed out");
        complete(r);
    });
    timeout->start(120000);
    process->start(ffprobe, args);
}

} // namespace tt
