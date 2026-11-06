#pragma once

#include "Settings.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimeEdit;

namespace tt {
class HiDriveClient;
struct ToolLocation;
}

class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

    void load(const tt::Settings &settings);
    [[nodiscard]] tt::Settings collect() const;
    /// Shows which ffmpeg is in use (or why none is usable) under the tool paths.
    void showToolLocation(const tt::ToolLocation &location);

signals:
    void applyRequested(const tt::Settings &settings);
    void downloadToolsRequested();

private:
    QWidget *pathRow(QLineEdit *edit, bool directory, const QString &filter = {});
    void testConnection();
    void checkTools();

    tt::Settings m_loaded;
    tt::HiDriveClient *m_testClient = nullptr;

    QLineEdit *m_link = nullptr;
    QLineEdit *m_linkFile = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_modifyRemote = nullptr;
    QLabel *m_testResult = nullptr;

    QSpinBox *m_poll = nullptr;
    QSpinBox *m_stable = nullptr;
    QLineEdit *m_extensions = nullptr;
    QPlainTextEdit *m_excludes = nullptr;

    QLineEdit *m_db = nullptr;
    QLineEdit *m_work = nullptr;
    QLineEdit *m_backup = nullptr;
    QSpinBox *m_backupDays = nullptr;
    QLineEdit *m_output = nullptr;
    QLineEdit *m_logDir = nullptr;

    QLineEdit *m_ffmpeg = nullptr;
    QLineEdit *m_ffprobe = nullptr;
    QLabel *m_toolsResult = nullptr;
    QPushButton *m_checkTools = nullptr;

    QSpinBox *m_crf = nullptr;
    QComboBox *m_preset = nullptr;
    QTableWidget *m_rules = nullptr;
    QDoubleSpinBox *m_fpsFactor = nullptr;
    QDoubleSpinBox *m_bufFactor = nullptr;
    QSpinBox *m_minGain = nullptr;

    QCheckBox *m_lowPriority = nullptr;
    QCheckBox *m_window = nullptr;
    QTimeEdit *m_windowStart = nullptr;
    QTimeEdit *m_windowEnd = nullptr;

    QComboBox *m_language = nullptr;
    QCheckBox *m_autostart = nullptr;
    QCheckBox *m_tray = nullptr;
    QCheckBox *m_notifications = nullptr;
};
