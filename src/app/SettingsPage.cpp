#include "SettingsPage.h"

#include "HiDriveClient.h"
#include "ShareLink.h"
#include "Tools.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeEdit>
#include <QToolButton>
#include <QVBoxLayout>

using namespace tt;

namespace {
QDoubleSpinBox *mbpsSpin(QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(0.1, 500.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.5);
    spin->setSuffix(QStringLiteral(" Mbit/s"));
    spin->setFrame(false);
    return spin;
}
} // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
    , m_testClient(new HiDriveClient(this))
{
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    // HiDrive
    auto *hidrive = new QGroupBox(tr("HiDrive"), content);
    auto *hf = new QFormLayout(hidrive);
    m_link = new QLineEdit(hidrive);
    m_link->setEchoMode(QLineEdit::Password);
    m_link->setPlaceholderText(QStringLiteral("https://my.hidrive.com/share/…#$/folder"));
    QAction *show = m_link->addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView), QLineEdit::TrailingPosition);
    show->setCheckable(true);
    show->setToolTip(tr("Show link"));
    connect(show, &QAction::toggled, this,
            [this](bool on) { m_link->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    hf->addRow(tr("Share link:"), m_link);
    m_linkFile = new QLineEdit(hidrive);
    m_linkFile->setPlaceholderText(tr("optional: file that contains the share link"));
    hf->addRow(tr("Link file:"), pathRow(m_linkFile, false, tr("Text files (*.txt);;All files (*)")));
    m_password = new QLineEdit(hidrive);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("only if the share is password protected"));
    hf->addRow(tr("Share password:"), m_password);
    m_modifyRemote = new QCheckBox(tr("Replace originals on HiDrive"), hidrive);
    m_modifyRemote->setToolTip(tr("Off (read-only): transcodes are saved in the local output folder and HiDrive is never changed.\n"
                                  "On: the transcode is uploaded, the original is deleted on HiDrive and kept as a local backup."));
    hf->addRow(QString(), m_modifyRemote);
    auto *testRow = new QHBoxLayout;
    auto *test = new QPushButton(tr("Test connection"), hidrive);
    m_testResult = new QLabel(hidrive);
    m_testResult->setWordWrap(true);
    testRow->addWidget(test);
    testRow->addWidget(m_testResult, 1);
    hf->addRow(QString(), testRow);
    connect(test, &QPushButton::clicked, this, &SettingsPage::testConnection);
    layout->addWidget(hidrive);

    // Monitoring
    auto *monitor = new QGroupBox(tr("Monitoring"), content);
    auto *mf = new QFormLayout(monitor);
    m_poll = new QSpinBox(monitor);
    m_poll->setRange(1, 1440);
    m_poll->setSuffix(tr(" min"));
    mf->addRow(tr("Check HiDrive every:"), m_poll);
    m_stable = new QSpinBox(monitor);
    m_stable->setRange(0, 1440);
    m_stable->setSuffix(tr(" min"));
    m_stable->setToolTip(tr("New files are processed once they are unchanged across two checks, or right away if they are older than this."));
    mf->addRow(tr("Treat files as complete after:"), m_stable);
    m_extensions = new QLineEdit(monitor);
    mf->addRow(tr("Video extensions:"), m_extensions);
    m_excludes = new QPlainTextEdit(monitor);
    m_excludes->setPlaceholderText(tr("one pattern per line, relative to the monitored folder, e.g. Archive/*"));
    m_excludes->setFixedHeight(70);
    mf->addRow(tr("Exclude:"), m_excludes);
    layout->addWidget(monitor);

    // Encoding
    auto *encoding = new QGroupBox(tr("Encoding (x265, HEVC Main10, MP4)"), content);
    auto *ef = new QFormLayout(encoding);
    m_crf = new QSpinBox(encoding);
    m_crf->setRange(0, 51);
    m_crf->setToolTip(tr("Constant quality: lower = better quality and bigger files. 20–26 is typical."));
    ef->addRow(tr("Quality (CRF):"), m_crf);
    m_preset = new QComboBox(encoding);
    m_preset->addItems(x265Presets());
    m_preset->setToolTip(tr("Slower presets compress better at the same quality."));
    ef->addRow(tr("Preset:"), m_preset);
    m_rules = new QTableWidget(4, 3, encoding);
    m_rules->setHorizontalHeaderLabels({tr("Shorter side ≥"), tr("Transcode above"), tr("Max bitrate")});
    m_rules->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_rules->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_rules->setSelectionMode(QAbstractItemView::NoSelection);
    for (int row = 0; row < 4; ++row) {
        m_rules->setVerticalHeaderItem(row, new QTableWidgetItem(defaultResolutionRules()[row].label));
        auto *minSide = new QSpinBox(m_rules);
        minSide->setRange(0, 10000);
        minSide->setSuffix(QStringLiteral(" px"));
        minSide->setFrame(false);
        m_rules->setCellWidget(row, 0, minSide);
        m_rules->setCellWidget(row, 1, mbpsSpin(m_rules));
        m_rules->setCellWidget(row, 2, mbpsSpin(m_rules));
    }
    const int rowHeight = std::max(m_rules->verticalHeader()->defaultSectionSize(),
                                   m_rules->cellWidget(0, 1)->sizeHint().height() + 4);
    m_rules->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_rules->verticalHeader()->setDefaultSectionSize(rowHeight);
    m_rules->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rules->setFixedHeight(m_rules->horizontalHeader()->sizeHint().height() + 4 * rowHeight + 2 * m_rules->frameWidth() + 2);
    ef->addRow(tr("Bitrates (≤ 30 fps):"), m_rules);
    m_fpsFactor = new QDoubleSpinBox(encoding);
    m_fpsFactor->setRange(1.0, 4.0);
    m_fpsFactor->setSingleStep(0.1);
    m_fpsFactor->setPrefix(QStringLiteral("× "));
    ef->addRow(tr("Factor for 50/60 fps:"), m_fpsFactor);
    m_bufFactor = new QDoubleSpinBox(encoding);
    m_bufFactor->setRange(1.0, 4.0);
    m_bufFactor->setSingleStep(0.5);
    m_bufFactor->setPrefix(QStringLiteral("× "));
    m_bufFactor->setToolTip(tr("VBV buffer size relative to the max bitrate."));
    ef->addRow(tr("VBV buffer:"), m_bufFactor);
    m_minGain = new QSpinBox(encoding);
    m_minGain->setRange(0, 90);
    m_minGain->setSuffix(QStringLiteral(" %"));
    m_minGain->setToolTip(tr("Keep the original if the transcode is not at least this much smaller."));
    ef->addRow(tr("Minimum saving:"), m_minGain);
    layout->addWidget(encoding);

    // Schedule
    auto *schedule = new QGroupBox(tr("Schedule"), content);
    auto *sf = new QFormLayout(schedule);
    m_lowPriority = new QCheckBox(tr("Run ffmpeg at low priority"), schedule);
    sf->addRow(QString(), m_lowPriority);
    m_window = new QCheckBox(tr("Only start encodes between"), schedule);
    m_windowStart = new QTimeEdit(schedule);
    m_windowEnd = new QTimeEdit(schedule);
    m_windowStart->setDisplayFormat(QStringLiteral("HH:mm"));
    m_windowEnd->setDisplayFormat(QStringLiteral("HH:mm"));
    auto *windowRow = new QHBoxLayout;
    windowRow->addWidget(m_window);
    windowRow->addWidget(m_windowStart);
    windowRow->addWidget(new QLabel(tr("and"), schedule));
    windowRow->addWidget(m_windowEnd);
    windowRow->addStretch();
    sf->addRow(QString(), windowRow);
    layout->addWidget(schedule);

    // Storage
    auto *storage = new QGroupBox(tr("Storage"), content);
    auto *stf = new QFormLayout(storage);
    m_db = new QLineEdit(storage);
    stf->addRow(tr("Database file:"), pathRow(m_db, false, tr("SQLite database (*.sqlite *.db);;All files (*)")));
    m_work = new QLineEdit(storage);
    stf->addRow(tr("Work folder:"), pathRow(m_work, true));
    m_backup = new QLineEdit(storage);
    stf->addRow(tr("Backup of originals:"), pathRow(m_backup, true));
    m_backupDays = new QSpinBox(storage);
    m_backupDays->setRange(0, 3650);
    m_backupDays->setSuffix(tr(" days"));
    stf->addRow(tr("Keep backups for:"), m_backupDays);
    m_output = new QLineEdit(storage);
    stf->addRow(tr("Output folder (read-only mode):"), pathRow(m_output, true));
    m_logDir = new QLineEdit(storage);
    stf->addRow(tr("Log folder:"), pathRow(m_logDir, true));
    layout->addWidget(storage);

    // Tools
    auto *tools = new QGroupBox(tr("Tools"), content);
    auto *tf = new QFormLayout(tools);
    m_ffmpeg = new QLineEdit(tools);
    m_ffmpeg->setPlaceholderText(tr("automatic"));
    tf->addRow(QStringLiteral("ffmpeg:"), pathRow(m_ffmpeg, false, tr("Programs (*.exe);;All files (*)")));
    m_ffprobe = new QLineEdit(tools);
    m_ffprobe->setPlaceholderText(tr("automatic (next to ffmpeg)"));
    tf->addRow(QStringLiteral("ffprobe:"), pathRow(m_ffprobe, false, tr("Programs (*.exe);;All files (*)")));
    auto *toolsHint = new QLabel(tr("Leave both empty: TanzTrans finds ffmpeg by itself, or downloads it with the "
                                    "button below."),
                                 tools);
    toolsHint->setWordWrap(true);
    tf->addRow(QString(), toolsHint);
    auto *toolsRow = new QHBoxLayout;
    m_checkTools = new QPushButton(tr("Check ffmpeg"), tools);
    auto *download = new QPushButton(tr("Download ffmpeg…"), tools);
    toolsRow->addWidget(m_checkTools);
    toolsRow->addWidget(download);
    toolsRow->addStretch();
    tf->addRow(QString(), toolsRow);
    m_toolsResult = new QLabel(tools);
    m_toolsResult->setWordWrap(true);
    m_toolsResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tf->addRow(QString(), m_toolsResult);
    connect(m_checkTools, &QPushButton::clicked, this, &SettingsPage::checkTools);
    connect(download, &QPushButton::clicked, this, &SettingsPage::downloadToolsRequested);
    layout->addWidget(tools);

    // Application
    auto *app = new QGroupBox(tr("Application"), content);
    auto *af = new QFormLayout(app);
    m_language = new QComboBox(app);
    m_language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_language->addItem(QStringLiteral("Deutsch"), QStringLiteral("de"));
    af->addRow(tr("Language:"), m_language);
    m_autostart = new QCheckBox(tr("Start with Windows (minimized to the tray)"), app);
    m_tray = new QCheckBox(tr("Closing the window keeps TanzTrans running in the tray"), app);
    m_notifications = new QCheckBox(tr("Show notifications"), app);
    af->addRow(QString(), m_autostart);
    af->addRow(QString(), m_tray);
    af->addRow(QString(), m_notifications);
    layout->addWidget(app);
    layout->addStretch();

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *revert = new QPushButton(tr("Revert"), this);
    auto *apply = new QPushButton(tr("Apply"), this);
    apply->setDefault(true);
    buttons->addWidget(revert);
    buttons->addWidget(apply);
    outer->addLayout(buttons);
    connect(revert, &QPushButton::clicked, this, [this] { load(m_loaded); });
    connect(apply, &QPushButton::clicked, this, [this] { emit applyRequested(collect()); });
}

QWidget *SettingsPage::pathRow(QLineEdit *edit, bool directory, const QString &filter)
{
    auto *row = new QWidget(edit->parentWidget());
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    edit->setParent(row);
    h->addWidget(edit);
    auto *browse = new QToolButton(row);
    browse->setText(QStringLiteral("…"));
    h->addWidget(browse);
    connect(browse, &QToolButton::clicked, this, [this, edit, directory, filter] {
        const QString current = edit->text();
        const QString chosen = directory ? QFileDialog::getExistingDirectory(this, QString(), current)
                                         : QFileDialog::getOpenFileName(this, QString(), current, filter);
        if (!chosen.isEmpty())
            edit->setText(QDir::toNativeSeparators(chosen));
    });
    return row;
}

void SettingsPage::load(const Settings &s)
{
    m_loaded = s;
    m_link->setText(s.shareLink);
    m_linkFile->setText(QDir::toNativeSeparators(s.linkFile));
    m_password->setText(s.sharePassword);
    m_modifyRemote->setChecked(s.modifyRemote);
    m_poll->setValue(s.pollMinutes);
    m_stable->setValue(s.stableMinutes);
    m_extensions->setText(s.extensions.join(QStringLiteral(", ")));
    m_excludes->setPlainText(s.excludes.join(QLatin1Char('\n')));
    m_db->setText(QDir::toNativeSeparators(s.dbPath));
    m_work->setText(QDir::toNativeSeparators(s.workDir));
    m_backup->setText(QDir::toNativeSeparators(s.backupDir));
    m_backupDays->setValue(s.backupDays);
    m_output->setText(QDir::toNativeSeparators(s.outputDir));
    m_logDir->setText(QDir::toNativeSeparators(s.logDir));
    const bool automatic = toolsAutomatic(s);
    m_ffmpeg->setText(automatic ? QString() : QDir::toNativeSeparators(s.ffmpegPath));
    m_ffprobe->setText(automatic ? QString() : QDir::toNativeSeparators(s.ffprobePath));
    m_crf->setValue(s.crf);
    m_preset->setCurrentText(s.preset);
    for (int row = 0; row < 4 && row < s.rules.size(); ++row) {
        static_cast<QSpinBox *>(m_rules->cellWidget(row, 0))->setValue(s.rules[row].minShortSide);
        static_cast<QDoubleSpinBox *>(m_rules->cellWidget(row, 1))->setValue(s.rules[row].thresholdMbps);
        static_cast<QDoubleSpinBox *>(m_rules->cellWidget(row, 2))->setValue(s.rules[row].capMbps);
    }
    m_fpsFactor->setValue(s.highFpsFactor);
    m_bufFactor->setValue(s.bufsizeFactor);
    m_minGain->setValue(s.minGainPercent);
    m_lowPriority->setChecked(s.lowPriority);
    m_window->setChecked(s.windowEnabled);
    m_windowStart->setTime(s.windowStart);
    m_windowEnd->setTime(s.windowEnd);
    m_language->setCurrentIndex(std::max(0, m_language->findData(s.language)));
    m_autostart->setChecked(s.autostart);
    m_tray->setChecked(s.minimizeToTray);
    m_notifications->setChecked(s.notifications);
}

Settings SettingsPage::collect() const
{
    Settings s = m_loaded;
    s.shareLink = m_link->text().trimmed();
    s.linkFile = QDir::fromNativeSeparators(m_linkFile->text().trimmed());
    s.sharePassword = m_password->text();
    s.modifyRemote = m_modifyRemote->isChecked();
    s.pollMinutes = m_poll->value();
    s.stableMinutes = m_stable->value();
    s.extensions.clear();
    for (const QString &ext : m_extensions->text().split(QRegularExpression(QStringLiteral("[,; ]+")), Qt::SkipEmptyParts))
        s.extensions << ext.trimmed().toLower().remove(QLatin1Char('.'));
    s.excludes = m_excludes->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    s.dbPath = QDir::fromNativeSeparators(m_db->text().trimmed());
    s.workDir = QDir::fromNativeSeparators(m_work->text().trimmed());
    s.backupDir = QDir::fromNativeSeparators(m_backup->text().trimmed());
    s.backupDays = m_backupDays->value();
    s.outputDir = QDir::fromNativeSeparators(m_output->text().trimmed());
    s.logDir = QDir::fromNativeSeparators(m_logDir->text().trimmed());
    s.ffmpegPath = QDir::fromNativeSeparators(m_ffmpeg->text().trimmed());
    s.ffprobePath = QDir::fromNativeSeparators(m_ffprobe->text().trimmed());
    s.crf = m_crf->value();
    s.preset = m_preset->currentText();
    for (int row = 0; row < 4 && row < s.rules.size(); ++row) {
        s.rules[row].minShortSide = static_cast<QSpinBox *>(m_rules->cellWidget(row, 0))->value();
        s.rules[row].thresholdMbps = static_cast<QDoubleSpinBox *>(m_rules->cellWidget(row, 1))->value();
        s.rules[row].capMbps = static_cast<QDoubleSpinBox *>(m_rules->cellWidget(row, 2))->value();
    }
    s.highFpsFactor = m_fpsFactor->value();
    s.bufsizeFactor = m_bufFactor->value();
    s.minGainPercent = m_minGain->value();
    s.lowPriority = m_lowPriority->isChecked();
    s.windowEnabled = m_window->isChecked();
    s.windowStart = m_windowStart->time();
    s.windowEnd = m_windowEnd->time();
    s.language = m_language->currentData().toString();
    s.autostart = m_autostart->isChecked();
    s.minimizeToTray = m_tray->isChecked();
    s.notifications = m_notifications->isChecked();
    return s;
}

void SettingsPage::testConnection()
{
    const Settings s = collect();
    QString error;
    const QString link = s.resolvedShareLink(&error);
    const auto parsed = parseShareLink(link);
    if (!parsed) {
        m_testResult->setText(QStringLiteral("<span style=\"color:#d32f2f\">%1</span>")
                                  .arg(error.isEmpty() ? tr("Not a valid HiDrive share link.") : error));
        return;
    }
    m_testResult->setText(tr("Connecting…"));
    m_testClient->configure(*parsed, s.sharePassword, false);
    const QString base = parsed->basePath;
    m_testClient->listDir(base, [this, base](Result<QList<RemoteEntry>> entries) {
        if (!entries) {
            m_testResult->setText(QStringLiteral("<span style=\"color:#d32f2f\">%1</span>").arg(entries.error().toHtmlEscaped()));
            return;
        }
        int dirs = 0;
        for (const RemoteEntry &e : *entries)
            dirs += e.isDir ? 1 : 0;
        m_testResult->setText(QStringLiteral("<span style=\"color:#2e7d32\">%1</span>")
                                  .arg(tr("OK – %1 contains %2 folder(s) and %3 file(s).")
                                           .arg(base.toHtmlEscaped())
                                           .arg(dirs)
                                           .arg(entries->size() - dirs)));
    });
}

void SettingsPage::checkTools()
{
    m_checkTools->setEnabled(false);
    m_toolsResult->setText(tr("Checking…"));
    // Tests the paths as currently entered (not yet applied).
    locateToolsAsync(collect(), this, [this](const ToolLocation &location) {
        m_checkTools->setEnabled(true);
        showToolLocation(location);
    });
}

void SettingsPage::showToolLocation(const ToolLocation &location)
{
    if (location.ok()) {
        m_toolsResult->setText(QStringLiteral("<span style=\"color:#2e7d32\">✔ %1</span><br>%2 (%3)")
                                   .arg(location.version.toHtmlEscaped(),
                                        QDir::toNativeSeparators(location.ffmpeg).toHtmlEscaped(),
                                        location.origin.toHtmlEscaped()));
    } else {
        m_toolsResult->setText(QStringLiteral("<span style=\"color:#d32f2f\">✘ %1</span>").arg(location.error.toHtmlEscaped()));
    }
}
