#include "FfmpegInstaller.h"

#include "Log.h"
#include "Tools.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QVersionNumber>

namespace tt {

namespace {

constexpr qint64 kRequiredSpace = 700LL * 1024 * 1024; // zip + unpacked + installed copy

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

QNetworkRequest request(const QString &url)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TanzTrans/" TT_VERSION));
    req.setTransferTimeout(60000); // no data for 60 s -> error
    return req;
}

} // namespace

QHash<QString, QString> parseChecksums(const QByteArray &text)
{
    static const QRegularExpression line(QStringLiteral(R"(^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$)"),
                                         QRegularExpression::MultilineOption);
    QHash<QString, QString> sums;
    for (auto it = line.globalMatch(QString::fromUtf8(text)); it.hasNext();) {
        const auto m = it.next();
        sums.insert(m.captured(2), m.captured(1).toLower());
    }
    return sums;
}

QString pickFfmpegAsset(const QStringList &names)
{
    const QString preferred = QString::fromLatin1(kFfmpegPreferredAsset);
    if (names.contains(preferred))
        return preferred;
    static const QRegularExpression release(QStringLiteral(R"(^ffmpeg-n(\d+)\.(\d+)-latest-win64-gpl-shared-\1\.\2\.zip$)"));
    QString best;
    QVersionNumber bestVersion;
    for (const QString &name : names) {
        const auto m = release.match(name);
        if (!m.hasMatch())
            continue;
        const QVersionNumber version(m.captured(1).toInt(), m.captured(2).toInt());
        if (best.isEmpty() || version > bestVersion) {
            best = name;
            bestVersion = version;
        }
    }
    return best;
}

FfmpegInstaller::FfmpegInstaller(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

FfmpegInstaller::~FfmpegInstaller()
{
    if (m_running) {
        m_running = false;
        cleanup();
    }
}

void FfmpegInstaller::start()
{
    if (m_running)
        return;
    m_running = true;
    m_workDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/ffmpeg-download");
    QDir(m_workDir).removeRecursively();
    if (!QDir().mkpath(m_workDir)) {
        fail(tr("Cannot create the folder %1.").arg(native(m_workDir)));
        return;
    }
    if (const QStorageInfo storage(m_workDir); storage.isValid() && storage.bytesAvailable() < kRequiredSpace) {
        fail(tr("Not enough free disk space: at least %1 MB are needed.").arg(kRequiredSpace / (1024 * 1024)));
        return;
    }
    emit stageChanged(tr("Looking for the current ffmpeg version…"));
    m_reply = m_network->get(request(QString::fromLatin1(kFfmpegReleaseUrl) + QStringLiteral("checksums.sha256")));
    connect(m_reply, &QNetworkReply::finished, this, &FfmpegInstaller::onChecksums);
}

void FfmpegInstaller::cancel()
{
    fail(tr("Cancelled."));
}

void FfmpegInstaller::onChecksums()
{
    QNetworkReply *reply = std::exchange(m_reply, nullptr);
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Cannot reach GitHub: %1").arg(reply->errorString()));
        return;
    }
    const QHash<QString, QString> sums = parseChecksums(reply->readAll());
    m_asset = pickFfmpegAsset(sums.keys());
    if (m_asset.isEmpty()) {
        fail(tr("No suitable ffmpeg build was found on GitHub."));
        return;
    }
    m_expectedHash = sums.value(m_asset);
    m_zip.setFileName(m_workDir + QLatin1Char('/') + m_asset);
    if (!m_zip.open(QIODevice::WriteOnly)) {
        fail(tr("Cannot write %1.").arg(native(m_zip.fileName())));
        return;
    }
    m_hash.reset();
    logInfo(tr("Downloading ffmpeg (%1).").arg(m_asset));
    emit stageChanged(tr("Downloading ffmpeg…"));
    m_reply = m_network->get(request(QString::fromLatin1(kFfmpegReleaseUrl) + m_asset));
    connect(m_reply, &QNetworkReply::readyRead, this, &FfmpegInstaller::onZipData);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &FfmpegInstaller::progress);
    connect(m_reply, &QNetworkReply::finished, this, &FfmpegInstaller::onZipFinished);
}

void FfmpegInstaller::onZipData()
{
    if (!m_reply)
        return;
    const QByteArray data = m_reply->readAll();
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return; // body of a redirect or an error page
    m_hash.addData(data);
    if (m_zip.write(data) != data.size())
        fail(tr("Cannot write %1.").arg(native(m_zip.fileName())));
}

void FfmpegInstaller::onZipFinished()
{
    onZipData();
    if (!m_running)
        return;
    QNetworkReply *reply = std::exchange(m_reply, nullptr);
    reply->deleteLater();
    m_zip.close();
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("The download failed: %1").arg(reply->errorString()));
        return;
    }
    if (QString::fromLatin1(m_hash.result().toHex()) != m_expectedHash) {
        fail(tr("The download is damaged (checksum mismatch). Please try again."));
        return;
    }

    emit stageChanged(tr("Unpacking…"));
    const QString tar = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")) + QStringLiteral("\\System32\\tar.exe");
    if (!QFileInfo::exists(tar)) {
        fail(tr("Windows' tar.exe is missing (Windows 10 version 1803 or newer is needed)."));
        return;
    }
    const QString unpackDir = m_workDir + QStringLiteral("/unpacked");
    QDir().mkpath(unpackDir);
    m_tar = new QProcess(this);
    // Relative zip path: keeps the command line ASCII even if the user name is not.
    m_tar->setWorkingDirectory(unpackDir);
    connect(m_tar, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        onUnpacked(status == QProcess::NormalExit ? code : -1);
    });
    connect(m_tar, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onUnpacked(-1);
    });
    m_tar->start(tar, {QStringLiteral("-xf"), QStringLiteral("../") + m_asset});
}

void FfmpegInstaller::onUnpacked(int exitCode)
{
    if (!m_running || !m_tar)
        return;
    std::exchange(m_tar, nullptr)->deleteLater();
    if (exitCode != 0) {
        fail(tr("Unpacking the download failed."));
        return;
    }
    install();
}

void FfmpegInstaller::install()
{
    QString bin;
    QDirIterator it(m_workDir + QStringLiteral("/unpacked"), {QStringLiteral("ffmpeg.exe")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && bin.isEmpty()) {
        const QFileInfo exe(it.next());
        if (QFileInfo::exists(exe.absolutePath() + QStringLiteral("/ffprobe.exe")))
            bin = exe.absolutePath();
    }
    if (bin.isEmpty()) {
        fail(tr("The download does not contain ffmpeg.exe and ffprobe.exe."));
        return;
    }

    const QString target = managedToolsDir();
    const QString fresh = target + QStringLiteral(".new");
    const QString old = target + QStringLiteral(".old");
    QDir(fresh).removeRecursively();
    QDir(old).removeRecursively();
    QDir().mkpath(fresh);
    for (const QFileInfo &file : QDir(bin).entryInfoList(QDir::Files)) {
        if (file.fileName().compare(QLatin1String("ffplay.exe"), Qt::CaseInsensitive) == 0)
            continue; // the player is not needed
        if (!QFile::rename(file.absoluteFilePath(), fresh + QLatin1Char('/') + file.fileName())) {
            QDir(fresh).removeRecursively();
            fail(tr("Cannot write to %1.").arg(native(fresh)));
            return;
        }
    }
    QFile::rename(QFileInfo(bin).absolutePath() + QStringLiteral("/LICENSE.txt"), fresh + QStringLiteral("/LICENSE.txt"));
    QFile source(fresh + QStringLiteral("/SOURCE.txt"));
    if (source.open(QIODevice::WriteOnly | QIODevice::Text)) {
        source.write(QStringLiteral("Downloaded by TanzTrans from %1%2\nSHA-256: %3\n"
                                    "Licence: GNU GPL v3, see LICENSE.txt. Source code: https://ffmpeg.org and "
                                    "https://github.com/BtbN/FFmpeg-Builds\n")
                         .arg(QString::fromLatin1(kFfmpegReleaseUrl), m_asset, m_expectedHash)
                         .toUtf8());
    }

    if (QFileInfo::exists(target) && !QDir().rename(target, old)) {
        QDir(fresh).removeRecursively();
        fail(tr("The current ffmpeg in %1 is in use. Try again when no video is being converted.").arg(native(target)));
        return;
    }
    if (!QDir().rename(fresh, target)) {
        QDir().rename(old, target);
        fail(tr("Cannot install ffmpeg into %1.").arg(native(target)));
        return;
    }
    QDir(old).removeRecursively();
    m_running = false;
    cleanup();
    logInfo(tr("ffmpeg installed in %1.").arg(native(target)));
    emit finished({});
}

void FfmpegInstaller::fail(const QString &error)
{
    if (!m_running)
        return;
    m_running = false;
    cleanup();
    logWarning(tr("ffmpeg download: %1").arg(error));
    emit finished(error);
}

void FfmpegInstaller::cleanup()
{
    if (QNetworkReply *reply = std::exchange(m_reply, nullptr)) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (QProcess *tar = std::exchange(m_tar, nullptr)) {
        tar->disconnect(this);
        tar->kill();
        tar->waitForFinished(3000);
        tar->deleteLater();
    }
    if (m_zip.isOpen())
        m_zip.close();
    if (!m_workDir.isEmpty())
        QDir(m_workDir).removeRecursively();
}

} // namespace tt
