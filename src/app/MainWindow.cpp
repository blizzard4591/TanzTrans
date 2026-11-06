#include "MainWindow.h"

#include "AppIcon.h"
#include "Log.h"
#include "LogView.h"
#include "QueueModel.h"
#include "SettingsPage.h"
#include "ToolsDialog.h"
#include "Translation.h"
#include "WinUtil.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

using namespace tt;

MainWindow::MainWindow(JobManager *jobs, const QString &iniPath, const QStringList &autostartArgs, QWidget *parent)
    : QMainWindow(parent)
    , m_jobs(jobs)
    , m_iniPath(iniPath)
    , m_autostartArgs(autostartArgs)
    , m_model(new QueueModel(jobs, this))
    , m_logBuffer(new LogBuffer(this))
{
    setWindowIcon(app::makeAppIcon());
    resize(1200, 720);
    {
        QSettings ini(m_iniPath, QSettings::IniFormat);
        restoreGeometry(ini.value(QStringLiteral("window/geometry")).toByteArray());
    }

    m_tray = new QSystemTrayIcon(app::makeAppIcon(), this);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            isVisible() ? hide() : showFromTray();
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
        if (!m_toolsPromptPending.isEmpty())
            showFromTray();
    });
    m_tray->show();

    connect(m_jobs, &JobManager::stateChanged, this, &MainWindow::updateState);
    connect(m_jobs, &JobManager::toolsMissing, this, [this] { checkTools(true); });
    connect(m_jobs, &JobManager::notify, this, [this](const QString &title, const QString &text, bool error) {
        if (m_jobs->settings().notifications)
            m_tray->showMessage(title, text, error ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 8000);
    });
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { m_selectionBeforeReset = selectedPath(); });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        const int row = m_model->rowOf(m_selectionBeforeReset);
        if (row >= 0 && m_table)
            m_table->selectRow(row);
    });
    connect(&m_clock, &QTimer::timeout, this, &MainWindow::updateState);
    m_clock.start(1000);

    buildUi();
    checkTools(true);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("TanzTrans"));

    // toolbar
    m_toolbar = addToolBar(tr("Controls"));
    m_toolbar->setObjectName(QStringLiteral("controls"));
    m_toolbar->setMovable(false);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_actScan = m_toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Scan now"));
    m_toolbar->addSeparator();
    m_actResume = m_toolbar->addAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Resume"));
    m_actPause = m_toolbar->addAction(style()->standardIcon(QStyle::SP_MediaPause), tr("Pause"));
    m_actPauseAfter = m_toolbar->addAction(tr("Pause after current job"));
    m_actPauseAfter->setCheckable(true);
    m_actStop = m_toolbar->addAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"));
    m_actPause->setToolTip(tr("Freeze the running encode immediately; Resume continues exactly where it stopped."));
    m_actStop->setToolTip(tr("Abort the running job; it stays in the queue and restarts from the beginning."));
    connect(m_actScan, &QAction::triggered, m_jobs, &JobManager::scanNow);
    connect(m_actResume, &QAction::triggered, m_jobs, &JobManager::resume);
    connect(m_actPause, &QAction::triggered, m_jobs, &JobManager::pause);
    connect(m_actPauseAfter, &QAction::toggled, m_jobs, &JobManager::setPauseAfterCurrent);
    connect(m_actStop, &QAction::triggered, m_jobs, &JobManager::stop);

    // tabs
    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);

    auto *queuePage = new QWidget(m_tabs);
    auto *ql = new QVBoxLayout(queuePage);
    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(tr("Show:"), queuePage));
    m_filter = new QComboBox(queuePage);
    m_filter->addItem(tr("All files"), int(JobManager::Filter::All));
    m_filter->addItem(tr("Queue and active"), int(JobManager::Filter::Active));
    m_filter->addItem(tr("Problems"), int(JobManager::Filter::Problems));
    m_filter->addItem(tr("Finished / nothing to do"), int(JobManager::Filter::Finished));
    m_filter->setCurrentIndex(m_filter->findData(int(m_model->filter())));
    filterRow->addWidget(m_filter);
    filterRow->addStretch();
    ql->addLayout(filterRow);
    connect(m_filter, &QComboBox::currentIndexChanged, this,
            [this] { m_model->setFilter(JobManager::Filter(m_filter->currentData().toInt())); });

    m_table = new QTableView(queuePage);
    m_table->setModel(m_model);
    m_table->setItemDelegateForColumn(QueueModel::ColProgress, new ProgressDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(QueueModel::ColFile, 230);
    m_table->setColumnWidth(QueueModel::ColFolder, 110);
    m_table->setColumnWidth(QueueModel::ColVideo, 190);
    m_table->setColumnWidth(QueueModel::ColBitrate, 95);
    m_table->setColumnWidth(QueueModel::ColSize, 85);
    m_table->setColumnWidth(QueueModel::ColStatus, 140);
    m_table->setColumnWidth(QueueModel::ColProgress, 120);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    ql->addWidget(m_table);
    m_tabs->addTab(queuePage, tr("Queue"));

    m_logView = new LogView(m_logBuffer, m_jobs->settings().logDir, m_tabs);
    m_tabs->addTab(m_logView, tr("Log"));

    m_settingsPage = new SettingsPage(m_tabs);
    m_settingsPage->load(m_jobs->settings());
    connect(m_settingsPage, &SettingsPage::applyRequested, this, &MainWindow::applySettings);
    connect(m_settingsPage, &SettingsPage::downloadToolsRequested, this, [this] { showToolsDialog({}, true); });
    if (!m_tools.ffmpeg.isEmpty() || !m_tools.error.isEmpty())
        m_settingsPage->showToolLocation(m_tools);
    m_tabs->addTab(m_settingsPage, tr("Settings"));

    // status bar
    m_modeLabel = new QLabel(this);
    m_stateLabel = new QLabel(this);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setFixedWidth(180);
    m_progress->setTextVisible(true);
    m_scanLabel = new QLabel(this);
    m_savedLabel = new QLabel(this);
    statusBar()->addWidget(m_modeLabel);
    statusBar()->addWidget(m_stateLabel, 1);
    statusBar()->addPermanentWidget(m_progress);
    statusBar()->addPermanentWidget(m_scanLabel);
    statusBar()->addPermanentWidget(m_savedLabel);

    // tray menu
    delete m_trayMenu;
    m_trayMenu = new QMenu(this);
    m_trayMenu->addAction(tr("Show TanzTrans"), this, &MainWindow::showFromTray);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(m_actResume);
    m_trayMenu->addAction(m_actPause);
    m_trayMenu->addAction(m_actStop);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(tr("Quit"), this, &MainWindow::quit);
    m_tray->setContextMenu(m_trayMenu);

    m_model->retranslate();
    updateState();
}

void MainWindow::rebuildUi()
{
    m_rebuildPending = false;
    const int tab = m_tabs ? m_tabs->currentIndex() : 0;
    removeToolBar(m_toolbar);
    m_toolbar->deleteLater();
    QWidget *central = takeCentralWidget();
    central->deleteLater();
    setStatusBar(new QStatusBar(this));
    m_table = nullptr;
    buildUi();
    m_tabs->setCurrentIndex(tab);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange && m_tabs && !m_rebuildPending) {
        m_rebuildPending = true;
        QMetaObject::invokeMethod(this, &MainWindow::rebuildUi, Qt::QueuedConnection);
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::updateState()
{
    if (!m_stateLabel)
        return;
    const auto state = m_jobs->runState();
    m_actResume->setEnabled(state != JobManager::RunState::Running);
    m_actPause->setEnabled(state == JobManager::RunState::Running);
    m_actStop->setEnabled(state != JobManager::RunState::Stopped);
    {
        const QSignalBlocker block(m_actPauseAfter);
        m_actPauseAfter->setChecked(m_jobs->pauseAfterCurrent());
    }
    m_actScan->setEnabled(!m_jobs->isScanning());

    const bool modify = m_jobs->settings().modifyRemote;
    m_modeLabel->setText(modify ? QStringLiteral("<b style=\"color:#d47b00\">%1</b>").arg(tr("Replace mode"))
                                : QStringLiteral("<b>%1</b>").arg(tr("Read-only")));
    m_modeLabel->setToolTip(modify ? tr("Originals on HiDrive are replaced after a verified transcode.")
                                   : tr("HiDrive is never modified; transcodes go to the local output folder."));
    m_stateLabel->setText(m_jobs->statusText());

    if (const FileRecord *r = m_jobs->encodingRecord(); r && r->progress >= 0) {
        m_progress->setVisible(true);
        m_progress->setValue(int(r->progress * 1000));
        m_progress->setFormat(r->etaSeconds >= 0 ? tr("%p % – %1 left").arg(formatDuration(r->etaSeconds)) : tr("%p %"));
    } else {
        m_progress->setVisible(false);
    }
    const int next = m_jobs->secondsToNextScan();
    m_scanLabel->setText(m_jobs->isScanning() ? tr("Scanning…")
                         : next >= 0          ? tr("Next scan in %1").arg(formatDuration(next))
                                              : QString());
    m_savedLabel->setText(tr("Saved: %1").arg(formatBytes(m_jobs->savedBytes())));

    QString tip = QStringLiteral("TanzTrans – ") + m_jobs->statusText();
    m_tray->setToolTip(tip.left(120));
}

QString MainWindow::selectedPath() const
{
    if (!m_table || !m_table->selectionModel())
        return {};
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    return rows.isEmpty() ? QString() : m_model->pathAt(rows.first().row());
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_table->indexAt(pos);
    if (!index.isValid())
        return;
    const QString path = m_model->pathAt(index.row());
    const FileRecord *r = m_jobs->record(path);
    if (!r)
        return;
    const FileStatus s = r->status;

    QMenu menu(this);
    if (s == FileStatus::Failed || s == FileStatus::NoGain || s == FileStatus::Missing)
        menu.addAction(tr("Retry"), this, [this, path] { m_jobs->retry(path); });
    if (r->probed && r->hasVideo
        && (s == FileStatus::SmallEnough || s == FileStatus::NoGain || s == FileStatus::Failed
            || s == FileStatus::AlreadyDone || s == FileStatus::Skipped || s == FileStatus::DoneLocal))
        menu.addAction(tr("Transcode anyway"), this, [this, path] { m_jobs->forceTranscode(path); });
    if (s == FileStatus::Skipped)
        menu.addAction(tr("Do not skip"), this, [this, path] { m_jobs->unskip(path); });
    else if (!isActive(s) && s != FileStatus::Encoded && s != FileStatus::Done && s != FileStatus::Output)
        menu.addAction(tr("Never transcode (skip)"), this, [this, path] { m_jobs->skip(path); });
    if (isQueueable(s)) {
        menu.addSeparator();
        menu.addAction(tr("Move to top"), this, [this, path] { m_jobs->moveToTop(path); });
        menu.addAction(tr("Move up"), this, [this, path] { m_jobs->moveUp(path); });
        menu.addAction(tr("Move down"), this, [this, path] { m_jobs->moveDown(path); });
    }
    menu.addSeparator();
    QAction *log = menu.addAction(tr("Open job log"), this, [r] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(r->logPath));
    });
    log->setEnabled(!r->logPath.isEmpty() && QFile::exists(r->logPath));
    QString local;
    for (const QString &candidate : {r->status == FileStatus::DoneLocal ? r->outPath : QString(), r->backupPath,
                                     r->localOutput, r->localSource}) {
        if (!candidate.isEmpty() && QFile::exists(candidate)) {
            local = candidate;
            break;
        }
    }
    QAction *reveal = menu.addAction(tr("Show local file in Explorer"), this, [local] {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,"), QDir::toNativeSeparators(local)});
    });
    reveal->setEnabled(!local.isEmpty());
    menu.addAction(tr("Copy HiDrive path"), this, [path] { QApplication::clipboard()->setText(path); });
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::applySettings(Settings settings)
{
    const Settings old = m_jobs->settings();
    if (settings.modifyRemote && !old.modifyRemote) {
        const auto answer = QMessageBox::warning(
            this, tr("Replace originals on HiDrive?"),
            tr("TanzTrans will upload each verified transcode, delete the original on HiDrive and keep the original "
               "only as a local backup for %n day(s).\n\nContinue?",
               "", settings.backupDays),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            settings.modifyRemote = false;
    }
    const bool requeue = settings.modifyRemote && !old.modifyRemote && m_jobs->localResultCount() > 0
                         && QMessageBox::question(
                                this, tr("Replace originals on HiDrive?"),
                                tr("%n video(s) were already converted in read-only mode; the results are in the "
                                   "local output folder. Convert them again and replace their originals on HiDrive too?",
                                   "", m_jobs->localResultCount()))
                                == QMessageBox::Yes;
    {
        QSettings ini(m_iniPath, QSettings::IniFormat);
        Settings stored = settings;
        if (!m_linkFileOverride.isEmpty() && stored.linkFile == m_linkFileOverride)
            stored.linkFile = Settings::load(ini).linkFile; // --link-file applies to this run only
        stored.save(ini);
    }
    if (settings.autostart != old.autostart)
        win::setAutostart(settings.autostart, QCoreApplication::applicationFilePath(), m_autostartArgs);
    if (settings.logDir != old.logDir)
        Log::instance().setLogFile(settings.logDir + QStringLiteral("/tanztrans.log"));
    m_jobs->applySettings(settings);
    if (requeue)
        m_jobs->requeueLocalResults();
    m_settingsPage->load(m_jobs->settings());
    if (settings.dbPath != old.dbPath)
        QMessageBox::information(this, QStringLiteral("TanzTrans"),
                                 tr("The new database location is used after TanzTrans is restarted."));
    if (settings.ffmpegPath != old.ffmpegPath || settings.ffprobePath != old.ffprobePath)
        checkTools(true);
    if (settings.language != old.language)
        app::applyLanguage(settings.language); // triggers LanguageChange -> rebuildUi
    updateState();
}

void MainWindow::checkTools(bool prompt)
{
    const quint64 id = ++m_toolsCheck;
    locateToolsAsync(m_jobs->settings(), this, [this, id, prompt](const ToolLocation &location) {
        if (id != m_toolsCheck)
            return; // the settings changed meanwhile; a newer check is running
        m_tools = location;
        if (m_settingsPage)
            m_settingsPage->showToolLocation(location);
        if (location.ok()) {
            logInfo(tr("Using %1 – %2 (%3).")
                        .arg(location.version, QDir::toNativeSeparators(location.ffmpeg), location.origin));
            m_toolsPromptPending.clear();
            m_jobs->setTools(location.ffmpeg, location.ffprobe);
            return;
        }
        logWarning(location.error);
        m_jobs->setToolsMissing(location.error);
        if (!prompt || m_toolsDialog)
            return;
        if (isVisible()) {
            showToolsDialog(location.error, false);
        } else {
            m_toolsPromptPending = location.error;
            m_tray->showMessage(QStringLiteral("TanzTrans"),
                                tr("TanzTrans needs ffmpeg to convert videos. Click here to set it up."),
                                QSystemTrayIcon::Warning, 15000);
        }
    });
}

void MainWindow::showToolsDialog(const QString &reason, bool startDownload)
{
    m_toolsPromptPending.clear();
    if (!m_toolsDialog) {
        auto *dialog = new ToolsDialog(reason, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            Settings settings = m_jobs->settings();
            switch (dialog->outcome()) {
            case ToolsDialog::Outcome::Installed:
                if (toolsAutomatic(settings)) {
                    checkTools(true);
                } else { // explicit paths would hide the download
                    settings.ffmpegPath.clear();
                    settings.ffprobePath.clear();
                    applySettings(settings);
                }
                break;
            case ToolsDialog::Outcome::Chosen:
                settings.ffmpegPath = dialog->chosenFfmpeg();
                settings.ffprobePath.clear();
                applySettings(settings);
                break;
            case ToolsDialog::Outcome::Later: break;
            }
        });
        m_toolsDialog = dialog;
        dialog->open();
    }
    m_toolsDialog->raise();
    m_toolsDialog->activateWindow();
    if (startDownload)
        m_toolsDialog->startDownload();
}

void MainWindow::setCurrentTab(int index)
{
    m_tabs->setCurrentIndex(index);
}

void MainWindow::showFromTray()
{
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
    if (!m_toolsPromptPending.isEmpty())
        showToolsDialog(m_toolsPromptPending, false);
}

void MainWindow::quit()
{
    m_quitting = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_quitting && m_jobs->settings().minimizeToTray && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        event->ignore();
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_tray->showMessage(QStringLiteral("TanzTrans"), tr("TanzTrans keeps running in the tray. Use Quit in the tray menu to exit."),
                                QSystemTrayIcon::Information, 5000);
        }
        return;
    }
    {
        QSettings ini(m_iniPath, QSettings::IniFormat);
        ini.setValue(QStringLiteral("window/geometry"), saveGeometry());
    }
    event->accept();
    m_tray->hide();
    QApplication::quit();
}
