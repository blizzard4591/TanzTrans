#pragma once

#include "Probe.h"
#include "Settings.h"

namespace tt {

struct Decision
{
    enum class Kind { Transcode, SmallEnough, AlreadyOurs, NotVideo };
    Kind kind = Kind::NotVideo;
    int ruleIndex = -1;
    bool highFps = false;
    qint64 thresholdBps = 0;
    qint64 capBps = 0;
};

/// Index of the rule with the highest minShortSide that shortSide reaches (any order);
/// below every rule: the rule with the lowest minShortSide. -1 only if there are no rules.
int ruleIndexForShortSide(const QList<ResolutionRule> &rules, int shortSide);

Decision decide(int shortSide, double fps, qint64 videoBitrate, bool hasVideo, bool taggedByUs, const Settings &s);

inline Decision decide(const ProbeResult &p, const Settings &s)
{
    return decide(p.shortSide(), p.fps, p.videoBitrate, p.hasVideo, p.taggedByUs(), s);
}

struct VerifyResult
{
    bool ok = false;
    bool noGain = false;
    QString message;
};

/// Checks an encoded output against its source before the original may be replaced.
VerifyResult verifyOutput(const ProbeResult &source, const ProbeResult &output, qint64 sourceSize, qint64 outputSize,
                          int minGainPercent);

} // namespace tt
