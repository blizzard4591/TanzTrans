#pragma once

#include "JobManager.h"
#include "Tools.h"

#include <QMainWindow>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>

class QAction;
class QComboBox;
class QLabel;
class QMenu;
class QProgressBar;
class QTabWidget;
class QTableView;
class LogBuffer;
class LogView;
class QueueModel;
class SettingsPage;
class ToolsDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(tt::JobManager *jobs, const QString &iniPath, const QStringList &autostartArgs, QWidget *parent = nullptr);

    void showFromTray();
    void setCurrentTab(int index);
    /// The link file given with --link-file; it is not written into the settings file.
    void setLinkFileOverride(const QString &path) { m_linkFileOverride = path; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void rebuildUi();
    void updateState();
    void showContextMenu(const QPoint &pos);
    void applySettings(tt::Settings settings);
    /// Finds ffmpeg in the background and hands it to the JobManager; if none is usable
    /// and `prompt` is set, offers the download (dialog, or a tray message while hidden).
    void checkTools(bool prompt);
    void showToolsDialog(const QString &reason, bool startDownload);
    void quit();
    [[nodiscard]] QString selectedPath() const;

    tt::JobManager *m_jobs;
    QString m_iniPath;
    QString m_linkFileOverride;
    QStringList m_autostartArgs;
    QueueModel *m_model = nullptr;
    LogBuffer *m_logBuffer = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;

    // rebuilt on language change
    QToolBar *m_toolbar = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTableView *m_table = nullptr;
    QComboBox *m_filter = nullptr;
    SettingsPage *m_settingsPage = nullptr;
    LogView *m_logView = nullptr;
    QAction *m_actScan = nullptr;
    QAction *m_actPause = nullptr;
    QAction *m_actPauseAfter = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actResume = nullptr;
    QLabel *m_stateLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_scanLabel = nullptr;
    QLabel *m_savedLabel = nullptr;
    QLabel *m_modeLabel = nullptr;

    QTimer m_clock;
    QPointer<ToolsDialog> m_toolsDialog;
    tt::ToolLocation m_tools;
    QString m_toolsPromptPending; ///< reason to show once the window becomes visible
    quint64 m_toolsCheck = 0;     ///< id of the newest tools check; older results are ignored
    QString m_selectionBeforeReset;
    bool m_quitting = false;
    bool m_trayHintShown = false;
    bool m_rebuildPending = false;
};
