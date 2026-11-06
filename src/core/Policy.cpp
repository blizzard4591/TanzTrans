#include "Policy.h"

#include <cmath>

namespace tt {

int ruleIndexForShortSide(const QList<ResolutionRule> &rules, int shortSide)
{
    int best = -1;   // highest minShortSide that the video reaches
    int lowest = -1; // fallback for videos below every rule
    for (int i = 0; i < int(rules.size()); ++i) {
        if (shortSide >= rules[i].minShortSide && (best < 0 || rules[i].minShortSide > rules[best].minShortSide))
            best = i;
        if (lowest < 0 || rules[i].minShortSide < rules[lowest].minShortSide)
            lowest = i;
    }
    return best >= 0 ? best : lowest;
}

Decision decide(int shortSide, double fps, qint64 videoBitrate, bool hasVideo, bool taggedByUs, const Settings &s)
{
    Decision d;
    if (!hasVideo) {
        d.kind = Decision::Kind::NotVideo;
        return d;
    }
    d.ruleIndex = ruleIndexForShortSide(s.rules, shortSide);
    d.highFps = fps > 30.5;
    if (d.ruleIndex >= 0) {
        const ResolutionRule &rule = s.rules[d.ruleIndex];
        const double factor = d.highFps ? s.highFpsFactor : 1.0;
        d.thresholdBps = qint64(std::llround(rule.thresholdMbps * factor * 1e6));
        d.capBps = qint64(std::llround(rule.capMbps * factor * 1e6));
    }
    if (taggedByUs)
        d.kind = Decision::Kind::AlreadyOurs;
    else if (videoBitrate > d.thresholdBps)
        d.kind = Decision::Kind::Transcode;
    else
        d.kind = Decision::Kind::SmallEnough;
    return d;
}

VerifyResult verifyOutput(const ProbeResult &source, const ProbeResult &output, qint64 sourceSize, qint64 outputSize,
                          int minGainPercent)
{
    VerifyResult v;
    if (!output.ok()) {
        v.message = QStringLiteral("cannot probe output: %1").arg(output.error);
        return v;
    }
    if (!output.hasVideo || output.vcodec != QLatin1String("hevc")) {
        v.message = QStringLiteral("output has no HEVC video stream");
        return v;
    }
    if (!output.vprofile.contains(QLatin1String("Main 10"))) {
        v.message = QStringLiteral("output profile is '%1', expected Main 10").arg(output.vprofile);
        return v;
    }
    if (output.vtag != QLatin1String("hvc1")) {
        v.message = QStringLiteral("output codec tag is '%1', expected hvc1").arg(output.vtag);
        return v;
    }
    const double frame = source.fps > 0 ? 1.0 / source.fps : 0.04;
    const double tolerance = 0.5 + 2 * frame;
    if (std::abs(output.duration - source.duration) > tolerance) {
        v.message = QStringLiteral("duration mismatch: source %1 s, output %2 s")
                        .arg(source.duration, 0, 'f', 2)
                        .arg(output.duration, 0, 'f', 2);
        return v;
    }
    if (source.audioIndex >= 0 && output.audioStreams < 1) {
        v.message = QStringLiteral("output has no audio stream");
        return v;
    }
    const double limit = double(sourceSize) * (100.0 - minGainPercent) / 100.0;
    if (double(outputSize) > limit) {
        v.noGain = true;
        v.message = QStringLiteral("output is only %1 % smaller (minimum %2 %)")
                        .arg(sourceSize > 0 ? 100.0 * (1.0 - double(outputSize) / double(sourceSize)) : 0.0, 0, 'f', 1)
                        .arg(minGainPercent);
        return v;
    }
    v.ok = true;
    return v;
}

} // namespace tt
