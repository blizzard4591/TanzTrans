#include "QueueModel.h"

#include "ShareLink.h"

#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QPainter>
#include <QStyle>

using namespace tt;

QueueModel::QueueModel(JobManager *jobs, QObject *parent)
    : QAbstractTableModel(parent)
    , m_jobs(jobs)
{
    connect(jobs, &JobManager::recordsReset, this, &QueueModel::rebuild);
    connect(jobs, &JobManager::recordUpdated, this, &QueueModel::onRecordUpdated);
    rebuild();
}

void QueueModel::setFilter(JobManager::Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    rebuild();
}

QString QueueModel::pathAt(int row) const
{
    return row >= 0 && row < m_paths.size() ? m_paths[row] : QString();
}

int QueueModel::rowOf(const QString &path) const
{
    return m_rows.value(path, -1);
}

void QueueModel::retranslate()
{
    emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    if (!m_paths.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_paths.size()) - 1, ColumnCount - 1));
}

void QueueModel::rebuild()
{
    beginResetModel();
    m_paths = m_jobs->orderedPaths(m_filter);
    m_rows.clear();
    for (int i = 0; i < m_paths.size(); ++i)
        m_rows.insert(m_paths[i], i);
    endResetModel();
}

void QueueModel::onRecordUpdated(const QString &path)
{
    const int row = rowOf(path);
    if (row >= 0)
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

int QueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_paths.size());
}

int QueueModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QueueModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColFile: return tr("File");
    case ColFolder: return tr("Folder");
    case ColVideo: return tr("Video");
    case ColBitrate: return tr("Bitrate");
    case ColSize: return tr("Size");
    case ColStatus: return tr("Status");
    case ColProgress: return tr("Progress");
    case ColResult: return tr("Result");
    default: return {};
    }
}

QVariant QueueModel::data(const QModelIndex &index, int role) const
{
    const FileRecord *r = index.isValid() ? m_jobs->record(pathAt(index.row())) : nullptr;
    if (!r)
        return {};
    const int col = index.column();

    if (role == PathRole)
        return r->path;
    if (role == ProgressRole)
        return col == ColProgress ? r->progress : -1.0;
    if (role == Qt::TextAlignmentRole) {
        if (col == ColBitrate || col == ColSize)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ForegroundRole && col == ColStatus) {
        switch (r->status) {
        case FileStatus::Failed: return QColor(0xd3, 0x2f, 0x2f);
        case FileStatus::Done:
        case FileStatus::DoneLocal: return QColor(0x2e, 0x7d, 0x32);
        case FileStatus::Encoding:
        case FileStatus::Downloading:
        case FileStatus::Uploading: return QColor(0x15, 0x65, 0xc0);
        default: return {};
        }
    }

    const auto result = [r]() -> QString {
        switch (r->status) {
        case FileStatus::Done:
        case FileStatus::DoneLocal:
        case FileStatus::Encoded: {
            if (r->outSize <= 0)
                return {};
            const qint64 base = r->origSize > 0 ? r->origSize : r->size;
            const int gain = base > 0 ? int(100.0 * (1.0 - double(r->outSize) / double(base))) : 0;
            return QStringLiteral("%1 (−%2 %)").arg(formatBytes(r->outSize)).arg(gain);
        }
        case FileStatus::SmallEnough:
            return QueueModel::tr("limit %1").arg(formatMbps(r->thresholdBps));
        case FileStatus::Encoding:
            return r->etaSeconds >= 0 ? QueueModel::tr("%1 fps, %2 left")
                                            .arg(QLocale().toString(r->encodeFps, 'f', 1), formatDuration(r->etaSeconds))
                                      : QString();
        default:
            return r->message;
        }
    };

    if (role == Qt::DisplayRole) {
        switch (col) {
        case ColFile: return r->name();
        case ColFolder: {
            const QString rel = relativeToBase(m_jobs->basePath(), remoteParent(r->path));
            return rel.isEmpty() ? QStringLiteral("/") : rel;
        }
        case ColVideo: {
            if (!r->probed || !r->hasVideo)
                return {};
            QString text = QStringLiteral("%1×%2 %3p").arg(r->width).arg(r->height).arg(qRound(r->fps));
            if (!r->hdr.isEmpty())
                text += QLatin1Char(' ') + r->hdr;
            return text + QLatin1Char(' ') + r->vcodec.toUpper();
        }
        case ColBitrate: return r->probed && r->hasVideo ? formatMbps(r->vbitrate) : QString();
        case ColSize: return formatBytes(r->origSize > 0 ? r->origSize : r->size);
        case ColStatus: return statusDisplayName(r->status);
        case ColProgress:
            return r->progress >= 0 ? QStringLiteral("%1 %").arg(QLocale().toString(r->progress * 100, 'f', 1)) : QString();
        case ColResult: return result();
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (col) {
        case ColFile: return r->path;
        case ColStatus:
        case ColResult: {
            QStringList lines;
            if (!r->message.isEmpty())
                lines << r->message;
            if (!r->outPath.isEmpty())
                lines << tr("Output: %1").arg(QDir::toNativeSeparators(r->outPath));
            if (!r->backupPath.isEmpty())
                lines << tr("Backup: %1").arg(QDir::toNativeSeparators(r->backupPath));
            if (r->capBps > 0 && r->status != FileStatus::SmallEnough)
                lines << tr("Max bitrate: %1").arg(formatMbps(r->capBps));
            return lines.join(QLatin1Char('\n'));
        }
        default: return {};
        }
    }
    return {};
}

void ProgressDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const double value = index.data(QueueModel::ProgressRole).toDouble();
    if (value < 0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyle *style = option.widget ? option.widget->style() : QApplication::style();
    QStyleOptionViewItem panel = option;
    initStyleOption(&panel, index);
    panel.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &panel, painter, option.widget);

    QStyleOptionProgressBar bar;
    bar.rect = option.rect.adjusted(3, 3, -3, -3);
    bar.minimum = 0;
    bar.maximum = 1000;
    bar.progress = int(value * 1000);
    bar.text = QStringLiteral("%1 %").arg(QLocale().toString(value * 100, 'f', 1));
    bar.textVisible = true;
    bar.textAlignment = Qt::AlignCenter;
    bar.state = option.state | QStyle::State_Horizontal;
    bar.palette = option.palette;
    style->drawControl(QStyle::CE_ProgressBar, &bar, painter, option.widget);
}
