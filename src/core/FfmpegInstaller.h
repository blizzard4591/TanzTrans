#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace tt {

/// SHA-256 sums from a `sha256sum`-style file: file name -> lower-case hex digest.
QHash<QString, QString> parseChecksums(const QByteArray &text);

/// Picks the build to download from the release's file names: the pinned branch
/// (kFfmpegPreferredAsset) if present, otherwise the newest win64 GPL shared release
/// branch build. Empty if there is none.
QString pickFfmpegAsset(const QStringList &names);

inline constexpr auto kFfmpegReleaseUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/";
inline constexpr auto kFfmpegPreferredAsset = "ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip";

/// Downloads ffmpeg + ffprobe from the BtbN builds on GitHub into managedToolsDir():
/// checksum list -> zip (SHA-256 checked) -> unpacked with Windows' tar.exe -> the bin
/// folder and the licence are moved into place. No administrator rights needed.
class FfmpegInstaller : public QObject
{
    Q_OBJECT
public:
    explicit FfmpegInstaller(QObject *parent = nullptr);
    ~FfmpegInstaller() override;

    void start();
    void cancel();
    [[nodiscard]] bool isRunning() const { return m_running; }

signals:
    void stageChanged(const QString &text);
    void progress(qint64 received, qint64 total);
    /// `error` is empty on success.
    void finished(const QString &error);

private:
    void onChecksums();
    void onZipData();
    void onZipFinished();
    void onUnpacked(int exitCode);
    void install();
    void fail(const QString &error);
    void cleanup();

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QProcess *m_tar = nullptr;
    QFile m_zip;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_asset;
    QString m_expectedHash;
    QString m_workDir;
    bool m_running = false;
};

} // namespace tt
