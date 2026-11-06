#include "LogView.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>

using namespace tt;

LogBuffer::LogBuffer(QObject *parent)
    : QObject(parent)
{
    for (const Log::Entry &e : Log::instance().takeStartupEntries())
        m_entries.append({e.time, e.level, e.text});
    connect(&Log::instance(), &Log::message, this, [this](const QDateTime &time, LogLevel level, const QString &text) {
        m_entries.append({time, level, text});
        if (m_entries.size() > 5000)
            m_entries.remove(0, m_entries.size() - 5000);
        emit appended(m_entries.last());
    });
}

void LogBuffer::clear()
{
    m_entries.clear();
    emit cleared();
}

LogView::LogView(LogBuffer *buffer, const QString &logDir, QWidget *parent)
    : QWidget(parent)
    , m_buffer(buffer)
    , m_logDir(logDir)
{
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Show:"), this));
    m_level = new QComboBox(this);
    m_level->addItem(tr("Debug and above"), int(LogLevel::Debug));
    m_level->addItem(tr("Info and above"), int(LogLevel::Info));
    m_level->addItem(tr("Warnings and errors"), int(LogLevel::Warning));
    m_level->addItem(tr("Errors only"), int(LogLevel::Error));
    m_level->setCurrentIndex(1);
    bar->addWidget(m_level);
    bar->addStretch();
    auto *openFolder = new QPushButton(tr("Open log folder"), this);
    auto *clear = new QPushButton(tr("Clear"), this);
    bar->addWidget(openFolder);
    bar->addWidget(clear);
    layout->addLayout(bar);

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setMaximumBlockCount(5000);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(m_text);

    connect(m_level, &QComboBox::currentIndexChanged, this, &LogView::refill);
    connect(openFolder, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_logDir)); });
    connect(clear, &QPushButton::clicked, m_buffer, &LogBuffer::clear);
    connect(m_buffer, &LogBuffer::appended, this, &LogView::append);
    connect(m_buffer, &LogBuffer::cleared, m_text, &QPlainTextEdit::clear);
    refill();
}

LogLevel LogView::minimumLevel() const
{
    return LogLevel(m_level->currentData().toInt());
}

void LogView::refill()
{
    m_text->clear();
    for (const LogBuffer::Entry &e : m_buffer->entries())
        append(e);
}

void LogView::append(const LogBuffer::Entry &e)
{
    if (int(e.level) < int(minimumLevel()))
        return;
    QString color;
    switch (e.level) {
    case LogLevel::Debug: color = QStringLiteral("#888888"); break;
    case LogLevel::Info: break;
    case LogLevel::Warning: color = QStringLiteral("#d47b00"); break;
    case LogLevel::Error: color = QStringLiteral("#d32f2f"); break;
    }
    const QString line = QStringLiteral("%1  %2  %3")
                             .arg(e.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), logLevelName(e.level).leftJustified(5),
                                  e.text)
                             .toHtmlEscaped()
                             .replace(QLatin1Char(' '), QStringLiteral("&nbsp;"));
    QScrollBar *sb = m_text->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 4;
    m_text->appendHtml(color.isEmpty() ? line : QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color, line));
    if (atBottom)
        sb->setValue(sb->maximum());
}
