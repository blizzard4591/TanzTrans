#include "Settings.h"

#include "Log.h"
#include "WinUtil.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

namespace tt {

QList<ResolutionRule> defaultResolutionRules()
{
    return {
        {QStringLiteral("2160p"), 1800, 16.0, 12.0},
        {QStringLiteral("1440p"), 1300, 10.0, 8.0},
        {QStringLiteral("1080p"), 900, 6.0, 4.5},
        {QStringLiteral("<1080p"), 0, 3.0, 2.5},
    };
}

QStringList x265Presets()
{
    return {QStringLiteral("ultrafast"), QStringLiteral("superfast"), QStringLiteral("veryfast"),
            QStringLiteral("faster"),    QStringLiteral("fast"),      QStringLiteral("medium"),
            QStringLiteral("slow"),      QStringLiteral("slower"),    QStringLiteral("veryslow")};
}

Settings Settings::defaults()
{
    Settings s;
    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    s.dbPath = local + QStringLiteral("/tanztrans.sqlite");
    s.workDir = local + QStringLiteral("/work");
    s.backupDir = local + QStringLiteral("/backup");
    s.logDir = local + QStringLiteral("/logs");
    s.outputDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/TanzTrans");
    return s;
}

Settings Settings::load(QSettings &ini)
{
    const Settings d = defaults();
    Settings s = d;

    ini.beginGroup(QStringLiteral("hidrive"));
    const auto secret = [&ini](const QString &key, const QString &what) {
        const QString stored = ini.value(key).toString();
        const QString plain = win::unprotectString(stored);
        if (plain.isEmpty() && !stored.isEmpty())
            logWarning(QCoreApplication::translate("tt::Settings",
                                                   "The saved %1 cannot be decrypted (settings from another Windows "
                                                   "user or computer?). Please enter it again.")
                           .arg(what));
        return plain;
    };
    s.shareLink = secret(QStringLiteral("link"), QCoreApplication::translate("tt::Settings", "share link"));
    s.sharePassword = secret(QStringLiteral("password"), QCoreApplication::translate("tt::Settings", "share password"));
    s.linkFile = ini.value(QStringLiteral("linkFile")).toString();
    s.modifyRemote = ini.value(QStringLiteral("modifyRemote"), d.modifyRemote).toBool();
    ini.endGroup();

    ini.beginGroup(QStringLiteral("monitor"));
    s.pollMinutes = ini.value(QStringLiteral("pollMinutes"), d.pollMinutes).toInt();
    s.stableMinutes = ini.value(QStringLiteral("stableMinutes"), d.stableMinutes).toInt();
    s.extensions = ini.value(QStringLiteral("extensions"), d.extensions).toStringList();
    s.excludes = ini.value(QStringLiteral("excludes"), d.excludes).toStringList();
    ini.endGroup();

    ini.beginGroup(QStringLiteral("storage"));
    s.dbPath = ini.value(QStringLiteral("database"), d.dbPath).toString();
    s.workDir = ini.value(QStringLiteral("workDir"), d.workDir).toString();
    s.backupDir = ini.value(QStringLiteral("backupDir"), d.backupDir).toString();
    s.outputDir = ini.value(QStringLiteral("outputDir"), d.outputDir).toString();
    s.logDir = ini.value(QStringLiteral("logDir"), d.logDir).toString();
    s.backupDays = ini.value(QStringLiteral("backupDays"), d.backupDays).toInt();
    ini.endGroup();

    ini.beginGroup(QStringLiteral("tools"));
    s.ffmpegPath = ini.value(QStringLiteral("ffmpeg"), d.ffmpegPath).toString();
    s.ffprobePath = ini.value(QStringLiteral("ffprobe"), d.ffprobePath).toString();
    ini.endGroup();

    ini.beginGroup(QStringLiteral("encoding"));
    s.crf = ini.value(QStringLiteral("crf"), d.crf).toInt();
    s.preset = ini.value(QStringLiteral("preset"), d.preset).toString();
    s.highFpsFactor = ini.value(QStringLiteral("highFpsFactor"), d.highFpsFactor).toDouble();
    s.bufsizeFactor = ini.value(QStringLiteral("bufsizeFactor"), d.bufsizeFactor).toDouble();
    s.minGainPercent = ini.value(QStringLiteral("minGainPercent"), d.minGainPercent).toInt();
    const int count = ini.beginReadArray(QStringLiteral("rules"));
    if (count == d.rules.size()) {
        for (int i = 0; i < count; ++i) {
            ini.setArrayIndex(i);
            s.rules[i].minShortSide = ini.value(QStringLiteral("minShortSide"), d.rules[i].minShortSide).toInt();
            s.rules[i].thresholdMbps = ini.value(QStringLiteral("thresholdMbps"), d.rules[i].thresholdMbps).toDouble();
            s.rules[i].capMbps = ini.value(QStringLiteral("capMbps"), d.rules[i].capMbps).toDouble();
        }
    }
    ini.endArray();
    ini.endGroup();

    ini.beginGroup(QStringLiteral("schedule"));
    s.lowPriority = ini.value(QStringLiteral("lowPriority"), d.lowPriority).toBool();
    s.windowEnabled = ini.value(QStringLiteral("windowEnabled"), d.windowEnabled).toBool();
    s.windowStart = QTime::fromString(ini.value(QStringLiteral("windowStart"), d.windowStart.toString(QStringLiteral("HH:mm"))).toString(), QStringLiteral("HH:mm"));
    s.windowEnd = QTime::fromString(ini.value(QStringLiteral("windowEnd"), d.windowEnd.toString(QStringLiteral("HH:mm"))).toString(), QStringLiteral("HH:mm"));
    if (!s.windowStart.isValid())
        s.windowStart = d.windowStart;
    if (!s.windowEnd.isValid())
        s.windowEnd = d.windowEnd;
    ini.endGroup();

    ini.beginGroup(QStringLiteral("app"));
    s.language = ini.value(QStringLiteral("language"), d.language).toString();
    s.autostart = ini.value(QStringLiteral("autostart"), d.autostart).toBool();
    s.minimizeToTray = ini.value(QStringLiteral("minimizeToTray"), d.minimizeToTray).toBool();
    s.notifications = ini.value(QStringLiteral("notifications"), d.notifications).toBool();
    ini.endGroup();

    return s;
}

void Settings::save(QSettings &ini) const
{
    ini.beginGroup(QStringLiteral("hidrive"));
    ini.setValue(QStringLiteral("link"), win::protectString(shareLink));
    ini.setValue(QStringLiteral("password"), win::protectString(sharePassword));
    ini.setValue(QStringLiteral("linkFile"), linkFile);
    ini.setValue(QStringLiteral("modifyRemote"), modifyRemote);
    ini.endGroup();

    ini.beginGroup(QStringLiteral("monitor"));
    ini.setValue(QStringLiteral("pollMinutes"), pollMinutes);
    ini.setValue(QStringLiteral("stableMinutes"), stableMinutes);
    ini.setValue(QStringLiteral("extensions"), extensions);
    ini.setValue(QStringLiteral("excludes"), excludes);
    ini.endGroup();

    ini.beginGroup(QStringLiteral("storage"));
    ini.setValue(QStringLiteral("database"), dbPath);
    ini.setValue(QStringLiteral("workDir"), workDir);
    ini.setValue(QStringLiteral("backupDir"), backupDir);
    ini.setValue(QStringLiteral("outputDir"), outputDir);
    ini.setValue(QStringLiteral("logDir"), logDir);
    ini.setValue(QStringLiteral("backupDays"), backupDays);
    ini.endGroup();

    ini.beginGroup(QStringLiteral("tools"));
    ini.setValue(QStringLiteral("ffmpeg"), ffmpegPath);
    ini.setValue(QStringLiteral("ffprobe"), ffprobePath);
    ini.endGroup();

    ini.beginGroup(QStringLiteral("encoding"));
    ini.setValue(QStringLiteral("crf"), crf);
    ini.setValue(QStringLiteral("preset"), preset);
    ini.setValue(QStringLiteral("highFpsFactor"), highFpsFactor);
    ini.setValue(QStringLiteral("bufsizeFactor"), bufsizeFactor);
    ini.setValue(QStringLiteral("minGainPercent"), minGainPercent);
    ini.beginWriteArray(QStringLiteral("rules"), int(rules.size()));
    for (int i = 0; i < rules.size(); ++i) {
        ini.setArrayIndex(i);
        ini.setValue(QStringLiteral("label"), rules[i].label);
        ini.setValue(QStringLiteral("minShortSide"), rules[i].minShortSide);
        ini.setValue(QStringLiteral("thresholdMbps"), rules[i].thresholdMbps);
        ini.setValue(QStringLiteral("capMbps"), rules[i].capMbps);
    }
    ini.endArray();
    ini.endGroup();

    ini.beginGroup(QStringLiteral("schedule"));
    ini.setValue(QStringLiteral("lowPriority"), lowPriority);
    ini.setValue(QStringLiteral("windowEnabled"), windowEnabled);
    ini.setValue(QStringLiteral("windowStart"), windowStart.toString(QStringLiteral("HH:mm")));
    ini.setValue(QStringLiteral("windowEnd"), windowEnd.toString(QStringLiteral("HH:mm")));
    ini.endGroup();

    ini.beginGroup(QStringLiteral("app"));
    ini.setValue(QStringLiteral("language"), language);
    ini.setValue(QStringLiteral("autostart"), autostart);
    ini.setValue(QStringLiteral("minimizeToTray"), minimizeToTray);
    ini.setValue(QStringLiteral("notifications"), notifications);
    ini.endGroup();
    ini.sync();
}

QString Settings::resolvedShareLink(QString *error) const
{
    if (!linkFile.trimmed().isEmpty()) {
        QFile file(linkFile.trimmed());
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("cannot read link file: %1").arg(file.errorString());
            return {};
        }
        return QString::fromUtf8(file.readAll()).trimmed();
    }
    return shareLink.trimmed();
}

} // namespace tt
