#pragma once

#include "Probe.h"
#include "Settings.h"

#include <QStringList>

namespace tt {

/// Value written into the output's "comment" tag. Files carrying it are never transcoded again.
QString encoderTag(const Settings &s, qint64 capBps);

/// ffmpeg arguments for one transcode (capped CRF, HEVC Main10, hvc1, metadata kept).
QStringList buildFfmpegArgs(const ProbeResult &source, qint64 capBps, const Settings &s, const QString &input,
                            const QString &output);

} // namespace tt
