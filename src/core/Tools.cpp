#include "Tools.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <memory>

namespace tt {

namespace {

/// lupdate files tr() calls outside a class under the namespace, so the context is "tt".
QString tr(const char *text)
{
    return QCoreApplication::translate("tt", text);
}

bool isBareName(const QString &path, QLatin1StringView name)
{
    const QString p = path.trimmed();
    return p.isEmpty() || p.compare(name, Qt::CaseInsensitive) == 0
           || p.compare(QString(name) + QStringLiteral(".exe"), Qt::CaseInsensitive) == 0;
}

/// Runs `program args` and returns its stdout, or nullopt if it could not be started or failed.
std::optional<QString> run(const QString &program, const QStringList &args)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(15000))
        return std::nullopt;
    if (!p.waitForFinished(30000)) {
        p.kill();
        p.waitForFinished(2000);
        return std::nullopt;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return std::nullopt;
    return QString::fromUtf8(p.readAllStandardOutput());
}

/// Tests one ffmpeg/ffprobe pair. Fills version or error.
ToolLocation test(const QString &ffmpeg, const QString &ffprobe, const QString &origin)
{
    ToolLocation loc;
    loc.ffmpeg = QDir::cleanPath(ffmpeg);
    loc.ffprobe = QDir::cleanPath(ffprobe);
    loc.origin = origin;
    const QString nativeFfmpeg = QDir::toNativeSeparators(loc.ffmpeg);
    if (!QFileInfo::exists(loc.ffmpeg)) {
        loc.error = tr("%1 does not exist.").arg(nativeFfmpeg);
        return loc;
    }
    if (!QFileInfo::exists(loc.ffprobe)) {
        loc.error = tr("ffprobe.exe is missing next to %1.").arg(nativeFfmpeg);
        return loc;
    }
    const auto version = run(loc.ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-version")});
    if (!version) {
        loc.error = tr("%1 cannot be started.").arg(nativeFfmpeg);
        return loc;
    }
    loc.version = version->section(QLatin1Char('\n'), 0, 0).section(QStringLiteral(" Copyright"), 0, 0).trimmed();
    const auto x265 = run(loc.ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-h"), QStringLiteral("encoder=libx265")});
    if (!x265 || !x265->contains(QLatin1String("Encoder libx265"))) {
        loc.error = tr("%1 has no H.265 encoder (libx265).").arg(nativeFfmpeg);
        return loc;
    }
    if (!x265->contains(QLatin1String("-dolbyvision"))) {
        loc.error = tr("%1 is too old; ffmpeg 7.1 or newer is needed.").arg(nativeFfmpeg);
        return loc;
    }
    if (!run(loc.ffprobe, {QStringLiteral("-hide_banner"), QStringLiteral("-version")})) {
        loc.error = tr("%1 cannot be started.").arg(QDir::toNativeSeparators(loc.ffprobe));
        return loc;
    }
    return loc;
}

/// Replaces %NAME% with the environment variable (registry PATH values are REG_EXPAND_SZ).
QString expandEnvironment(QString text)
{
    static const QRegularExpression var(QStringLiteral("%([^%]+)%"));
    for (auto it = var.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        if (qEnvironmentVariableIsSet(m.captured(1).toLocal8Bit().constData()))
            text.replace(m.captured(0), qEnvironmentVariable(m.captured(1).toLocal8Bit().constData()));
    }
    return text;
}

QStringList pathDirectories()
{
    QStringList dirs = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
#ifdef Q_OS_WIN
    // The process PATH is frozen at start; the registry has what a new install added since.
    for (const QString &key : {QStringLiteral("HKEY_CURRENT_USER\\Environment"),
                               QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment")}) {
        const QSettings reg(key, QSettings::NativeFormat);
        dirs += expandEnvironment(reg.value(QStringLiteral("Path")).toString()).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    }
#endif
    return dirs;
}

struct Candidate
{
    QString dir;
    QString origin;
};

QList<Candidate> candidateDirectories()
{
    QList<Candidate> list;
    QStringList seen;
    const auto add = [&](const QString &dir, const QString &origin) {
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(dir.trimmed()));
        if (clean.isEmpty() || seen.contains(clean, Qt::CaseInsensitive))
            return;
        seen << clean;
        list.append({clean, origin});
    };
    add(QCoreApplication::applicationDirPath(), tr("next to TanzTrans"));
    add(managedToolsDir(), tr("downloaded by TanzTrans"));
    for (const QString &dir : pathDirectories())
        add(dir, tr("Windows PATH"));
    add(qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/Microsoft/WinGet/Links"), tr("winget"));
    add(qEnvironmentVariable("ProgramFiles") + QStringLiteral("/WinGet/Links"), tr("winget"));
    return list;
}

} // namespace

QString managedToolsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/ffmpeg");
}

bool toolsAutomatic(const Settings &settings)
{
    return isBareName(settings.ffmpegPath, QLatin1StringView("ffmpeg"))
           && isBareName(settings.ffprobePath, QLatin1StringView("ffprobe"));
}

ToolLocation locateTools(const Settings &settings)
{
    const QString exe = QStringLiteral(".exe");
    if (!toolsAutomatic(settings)) {
        QString ffmpeg = settings.ffmpegPath.trimmed();
        QString ffprobe = settings.ffprobePath.trimmed();
        if (isBareName(ffmpeg, QLatin1StringView("ffmpeg")))
            ffmpeg = QFileInfo(ffprobe).absolutePath() + QStringLiteral("/ffmpeg") + exe;
        if (isBareName(ffprobe, QLatin1StringView("ffprobe")))
            ffprobe = QFileInfo(ffmpeg).absolutePath() + QStringLiteral("/ffprobe") + exe;
        return test(ffmpeg, ffprobe, tr("settings"));
    }

    std::optional<ToolLocation> firstProblem;
    for (const Candidate &c : candidateDirectories()) {
        const QString ffmpeg = c.dir + QStringLiteral("/ffmpeg") + exe;
        if (!QFileInfo::exists(ffmpeg))
            continue;
        ToolLocation loc = test(ffmpeg, c.dir + QStringLiteral("/ffprobe") + exe, c.origin);
        if (loc.ok())
            return loc;
        if (!firstProblem)
            firstProblem = loc;
    }
    if (firstProblem)
        return *firstProblem;
    ToolLocation none;
    none.error = tr("ffmpeg was not found on this computer.");
    return none;
}

void locateToolsAsync(const Settings &settings, QObject *context, std::function<void(ToolLocation)> done)
{
    auto result = std::make_shared<ToolLocation>();
    QThread *thread = QThread::create([settings, result] { *result = locateTools(settings); });
    // `finished` is queued to `context`; the connection dies with `context`.
    QObject::connect(thread, &QThread::finished, context, [result, done = std::move(done)] { done(*result); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

} // namespace tt
