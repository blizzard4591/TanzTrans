#include "Encoder.h"

#include "WinUtil.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace tt {

Encoder::Encoder(QObject *parent)
    : QObject(parent)
{
}

Encoder::~Encoder()
{
    if (m_process) {
        m_process->disconnect(this);
        if (m_suspended)
            win::resumeProcess(m_process->processId());
        m_process->kill();
        m_process->waitForFinished(5000);
    }
}

bool Encoder::start(const QString &program, const QStringList &args, const QString &logPath, double durationSeconds,
                    bool lowPriority)
{
    if (m_process)
        return false;
    m_duration = durationSeconds;
    m_fraction = m_fps = m_speed = 0;
    m_suspended = m_aborted = m_startFailed = false;
    m_stdoutBuffer.clear();
    m_lastErrors.clear();

    QDir().mkpath(QFileInfo(logPath).absolutePath());
    m_log.setFileName(logPath);
    if (m_log.open(QIODevice::Append | QIODevice::Text)) {
        QStringList quoted;
        for (const QString &a : args)
            quoted << (a.contains(QLatin1Char(' ')) ? QLatin1Char('"') + a + QLatin1Char('"') : a);
        m_log.write(QStringLiteral("# %1 started\n# %2 %3\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), program, quoted.join(QLatin1Char(' ')))
                        .toUtf8());
        m_log.flush();
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    m_process->setCreateProcessArgumentsModifier([lowPriority](QProcess::CreateProcessArguments *a) {
        a->flags |= CREATE_NO_WINDOW;
        if (lowPriority)
            a->flags |= BELOW_NORMAL_PRIORITY_CLASS;
    });
#else
    Q_UNUSED(lowPriority);
#endif
    connect(m_process, &QProcess::started, this, [this] { win::killWithThisProcess(m_process->processId()); });
    connect(m_process, &QProcess::readyReadStandardOutput, this, &Encoder::readProgress);
    connect(m_process, &QProcess::readyReadStandardError, this, &Encoder::readStderr);
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readProgress();
        readStderr();
        const bool ok = !m_aborted && status == QProcess::NormalExit && code == 0;
        QString error;
        if (!ok && !m_aborted)
            error = QStringLiteral("ffmpeg exit code %1: %2").arg(code).arg(m_lastErrors.join(QStringLiteral(" | ")).right(600));
        finish(ok, error);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_startFailed = true;
            finish(false, QStringLiteral("cannot start ffmpeg: %1").arg(m_process->errorString()));
        }
    });
    m_process->start(program, args);
    return true;
}

void Encoder::finish(bool ok, const QString &error)
{
    if (!m_process)
        return;
    if (m_log.isOpen()) {
        m_log.write(QStringLiteral("# %1 finished: %2\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                             m_aborted ? QStringLiteral("aborted") : (ok ? QStringLiteral("ok") : error))
                        .toUtf8());
        m_log.close();
    }
    QProcess *process = std::exchange(m_process, nullptr);
    process->deleteLater();
    const bool aborted = m_aborted;
    m_suspended = false;
    emit finished(ok, aborted, error);
}

void Encoder::readProgress()
{
    if (!m_process)
        return;
    m_stdoutBuffer += m_process->readAllStandardOutput();
    qsizetype newline = 0;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        const qsizetype eq = line.indexOf('=');
        if (eq < 0)
            continue;
        const QByteArray key = line.left(eq);
        const QByteArray value = line.mid(eq + 1);
        if (key == "out_time_us" || key == "out_time_ms") { // both are microseconds in ffmpeg
            const double seconds = value.toDouble() / 1e6;
            if (m_duration > 0 && seconds >= 0)
                m_fraction = std::clamp(seconds / m_duration, 0.0, 1.0);
        } else if (key == "fps") {
            m_fps = value.toDouble();
        } else if (key == "speed") {
            m_speed = QByteArray(value).replace('x', "").trimmed().toDouble();
        } else if (key == "progress") {
            emit progress(m_fraction, m_fps, m_speed);
        }
    }
}

void Encoder::readStderr()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardError();
    if (data.isEmpty())
        return;
    if (m_log.isOpen()) {
        m_log.write(data);
        m_log.flush();
    }
    for (const QByteArray &line : data.split('\n')) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (text.isEmpty())
            continue;
        m_lastErrors << text;
        if (m_lastErrors.size() > 5)
            m_lastErrors.removeFirst();
    }
}

bool Encoder::suspend()
{
    if (!m_process || m_suspended)
        return false;
    m_suspended = win::suspendProcess(m_process->processId());
    return m_suspended;
}

bool Encoder::resume()
{
    if (!m_process || !m_suspended)
        return false;
    if (win::resumeProcess(m_process->processId()))
        m_suspended = false;
    return !m_suspended;
}

void Encoder::abort()
{
    if (!m_process)
        return;
    m_aborted = true;
    if (m_suspended)
        win::resumeProcess(m_process->processId());
    m_process->kill();
    m_process->waitForFinished(5000); // the output file is closed when this returns
}

} // namespace tt
