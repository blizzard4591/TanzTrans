#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

namespace tt {

enum class LogLevel { Debug = 0, Info, Warning, Error };

QString logLevelName(LogLevel level);

/// Application log. Every message passes through redact(), which replaces registered
/// secrets (share id, access token, password) by "<redacted>" before it is stored or shown.
class Log final : public QObject
{
    Q_OBJECT
public:
    static Log &instance();

    void addSecret(const QString &secret);
    [[nodiscard]] QString redact(QString text) const;

    void setLogFile(const QString &path);
    void write(LogLevel level, const QString &text);

    struct Entry
    {
        QDateTime time;
        LogLevel level;
        QString text;
    };
    /// Messages written before the GUI log view existed (startup). Returns them once;
    /// afterwards nothing more is kept.
    QList<Entry> takeStartupEntries();

signals:
    void message(const QDateTime &time, tt::LogLevel level, const QString &text);

private:
    Log() = default;

    mutable QMutex m_mutex;
    QStringList m_secrets;
    QString m_filePath;
    QList<Entry> m_startup;
    bool m_keepStartup = true;
};

inline void logDebug(const QString &text) { Log::instance().write(LogLevel::Debug, text); }
inline void logInfo(const QString &text) { Log::instance().write(LogLevel::Info, text); }
inline void logWarning(const QString &text) { Log::instance().write(LogLevel::Warning, text); }
inline void logError(const QString &text) { Log::instance().write(LogLevel::Error, text); }

} // namespace tt
