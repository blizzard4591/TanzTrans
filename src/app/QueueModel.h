#pragma once

#include "JobManager.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QStyledItemDelegate>

class QueueModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { ColFile, ColFolder, ColVideo, ColBitrate, ColSize, ColStatus, ColProgress, ColResult, ColumnCount };
    static constexpr int ProgressRole = Qt::UserRole + 1;
    static constexpr int PathRole = Qt::UserRole + 2;

    explicit QueueModel(tt::JobManager *jobs, QObject *parent = nullptr);

    void setFilter(tt::JobManager::Filter filter);
    [[nodiscard]] tt::JobManager::Filter filter() const { return m_filter; }
    [[nodiscard]] QString pathAt(int row) const;
    [[nodiscard]] int rowOf(const QString &path) const;
    void retranslate();

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    void rebuild();
    void onRecordUpdated(const QString &path);

    tt::JobManager *m_jobs;
    tt::JobManager::Filter m_filter = tt::JobManager::Filter::All;
    QStringList m_paths;
    QHash<QString, int> m_rows;
};

/// Paints QueueModel::ProgressRole as a progress bar.
class ProgressDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
