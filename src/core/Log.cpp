#include "Log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QUrl>

#include <utility>

namespace tt {

QString logLevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("DEBUG");
    case LogLevel::Info: return QStringLiteral("INFO");
    case LogLevel::Warning: return QStringLiteral("WARN");
    case LogLevel::Error: return QStringLiteral("ERROR");
    }
    return {};
}

Log &Log::instance()
{
    static Log log;
    return log;
}

void Log::addSecret(const QString &secret)
{
    if (secret.size() < 4)
        return;
    QMutexLocker lock(&m_mutex);
    for (const QString &variant : {secret, QString::fromLatin1(QUrl::toPercentEncoding(secret))}) {
        if (!m_secrets.contains(variant))
            m_secrets.append(variant);
    }
    // Longest first, so a secret that contains another one is replaced as a whole.
    std::ranges::sort(m_secrets, [](const QString &a, const QString &b) { return a.size() > b.size(); });
}

QString Log::redact(QString text) const
{
    QMutexLocker lock(&m_mutex);
    for (const QString &secret : m_secrets)
        text.replace(secret, QStringLiteral("<redacted>"));
    return text;
}

void Log::setLogFile(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    m_filePath = path;
    if (!path.isEmpty())
        QDir().mkpath(QFileInfo(path).absolutePath());
}

void Log::write(LogLevel level, const QString &raw)
{
    const QString text = redact(raw);
    const QDateTime now = QDateTime::currentDateTime();
    {
        QMutexLocker lock(&m_mutex);
        if (!m_filePath.isEmpty()) {
            if (QFileInfo(m_filePath).size() > 10 * 1024 * 1024) {
                QFile::remove(m_filePath + QStringLiteral(".1"));
                QFile::rename(m_filePath, m_filePath + QStringLiteral(".1"));
            }
            QFile file(m_filePath);
            if (file.open(QIODevice::Append | QIODevice::Text)) {
                file.write(QStringLiteral("%1 %2 %3\n")
                               .arg(now.toString(Qt::ISODateWithMs), logLevelName(level).leftJustified(5), text)
                               .toUtf8());
            }
        }
        if (m_keepStartup && m_startup.size() < 1000)
            m_startup.append({now, level, text});
    }
    emit message(now, level, text);
}

QList<Log::Entry> Log::takeStartupEntries()
{
    QMutexLocker lock(&m_mutex);
    m_keepStartup = false;
    return std::exchange(m_startup, {});
}

} // namespace tt
