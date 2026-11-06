#pragma once

#include <QFile>
#include <QObject>
#include <QProcess>
#include <QStringList>

namespace tt {

/// Runs one ffmpeg process, parses `-progress pipe:1` output and writes stderr to a job log.
class Encoder : public QObject
{
    Q_OBJECT
public:
    explicit Encoder(QObject *parent = nullptr);
    ~Encoder() override;

    bool start(const QString &program, const QStringList &args, const QString &logPath, double durationSeconds,
               bool lowPriority);
    [[nodiscard]] bool isRunning() const { return m_process != nullptr; }
    [[nodiscard]] bool isSuspended() const { return m_suspended; }
    /// The last start() failed because ffmpeg could not be started at all.
    [[nodiscard]] bool startFailed() const { return m_startFailed; }

    bool suspend();
    bool resume();
    void abort();

signals:
    void progress(double fraction, double fps, double speed);
    void finished(bool ok, bool aborted, const QString &error);

private:
    void readProgress();
    void readStderr();
    void finish(bool ok, const QString &error);

    QProcess *m_process = nullptr;
    QFile m_log;
    QByteArray m_stdoutBuffer;
    QStringList m_lastErrors;
    double m_duration = 0;
    double m_fraction = 0;
    double m_fps = 0;
    double m_speed = 0;
    bool m_suspended = false;
    bool m_aborted = false;
    bool m_startFailed = false;
};

} // namespace tt
