#pragma once

#include "Log.h"

#include <QDateTime>
#include <QList>
#include <QWidget>

class QComboBox;
class QPlainTextEdit;

/// Keeps the last log entries so a rebuilt LogView (language switch) can show them again.
class LogBuffer : public QObject
{
    Q_OBJECT
public:
    struct Entry
    {
        QDateTime time;
        tt::LogLevel level;
        QString text;
    };

    explicit LogBuffer(QObject *parent = nullptr);
    [[nodiscard]] const QList<Entry> &entries() const { return m_entries; }
    void clear();

signals:
    void appended(const LogBuffer::Entry &entry);
    void cleared();

private:
    QList<Entry> m_entries;
};

class LogView : public QWidget
{
    Q_OBJECT
public:
    LogView(LogBuffer *buffer, const QString &logDir, QWidget *parent = nullptr);

private:
    void refill();
    void append(const LogBuffer::Entry &entry);
    [[nodiscard]] tt::LogLevel minimumLevel() const;

    LogBuffer *m_buffer;
    QString m_logDir;
    QComboBox *m_level = nullptr;
    QPlainTextEdit *m_text = nullptr;
};
