#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>

class QSettings;

namespace tt {

/// One row of the "when to transcode" table. Bitrates are for <= 30 fps;
/// above that they are multiplied by Settings::highFpsFactor.
struct ResolutionRule
{
    QString label;
    int minShortSide = 0;      ///< rule applies if min(width, height) >= this
    double thresholdMbps = 0;  ///< transcode if the source video bitrate is above this
    double capMbps = 0;        ///< VBV max bitrate of the output
};

QList<ResolutionRule> defaultResolutionRules();

struct Settings
{
    // HiDrive
    QString shareLink;       ///< stored DPAPI-encrypted
    QString sharePassword;   ///< stored DPAPI-encrypted
    QString linkFile;        ///< if set, the share link is read from this file instead
    bool modifyRemote = false; ///< false = read-only: results go to outputDir

    // Monitoring
    int pollMinutes = 10;
    int stableMinutes = 30;  ///< files older than this are probed on first sight
    QStringList extensions{QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"),
                           QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("3gp")};
    QStringList excludes;    ///< glob patterns relative to the base folder

    // Storage
    QString dbPath;
    QString workDir;
    QString backupDir;
    QString outputDir;
    QString logDir;
    int backupDays = 30;

    // Tools: empty = find automatically (see locateTools() in Tools.h)
    QString ffmpegPath;
    QString ffprobePath;

    // Encoding
    int crf = 24;
    QString preset = QStringLiteral("slow");
    QList<ResolutionRule> rules = defaultResolutionRules();
    double highFpsFactor = 1.5;
    double bufsizeFactor = 2.0;
    int minGainPercent = 25;

    // Schedule
    bool lowPriority = true;
    bool windowEnabled = false;
    QTime windowStart{22, 0};
    QTime windowEnd{7, 0};

    // Application
    QString language = QStringLiteral("en");
    bool autostart = false;
    bool minimizeToTray = true;
    bool notifications = true;

    static Settings defaults();
    static Settings load(QSettings &ini);
    void save(QSettings &ini) const;

    /// The share link from linkFile (if set) or shareLink. Empty + error on failure.
    QString resolvedShareLink(QString *error = nullptr) const;
};

QStringList x265Presets();

} // namespace tt
