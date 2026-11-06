#include "ToolsDialog.h"

#include "FfmpegInstaller.h"
#include "Tools.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

using namespace tt;

ToolsDialog::ToolsDialog(const QString &reason, QWidget *parent)
    : QDialog(parent)
    , m_installer(new FfmpegInstaller(this))
{
    setWindowTitle(tr("TanzTrans needs ffmpeg"));
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QHBoxLayout;
    auto *icon = new QLabel(this);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(48, 48));
    icon->setAlignment(Qt::AlignTop);
    intro->addWidget(icon);
    auto *text = new QLabel(
        QStringLiteral("<b>%1</b><p>%2</p>")
            .arg(tr("TanzTrans needs ffmpeg"),
                 tr("TanzTrans converts the videos with the free program ffmpeg. It is not set up on this "
                    "computer yet. TanzTrans can download it for you – this takes a minute.")),
        this);
    text->setWordWrap(true);
    intro->addWidget(text, 1);
    layout->addLayout(intro);

    if (!reason.isEmpty()) {
        auto *details = new QLabel(tr("Details: %1").arg(reason), this);
        details->setWordWrap(true);
        details->setStyleSheet(QStringLiteral("color: palette(dark);"));
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(details);
    }

    m_download = new QPushButton(tr("Download ffmpeg automatically (about 85 MB)"), this);
    m_download->setDefault(true);
    m_download->setMinimumHeight(m_download->sizeHint().height() * 3 / 2);
    layout->addWidget(m_download);
    auto *note = new QLabel(tr("From GitHub (BtbN/FFmpeg-Builds) into %1. No administrator rights are needed; "
                               "nothing else on the computer is changed. ffmpeg is free software (GNU GPL).")
                                .arg(QDir::toNativeSeparators(managedToolsDir())),
                            this);
    note->setWordWrap(true);
    note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(note);

    m_stage = new QLabel(this);
    m_stage->setWordWrap(true);
    m_stage->setVisible(false);
    layout->addWidget(m_stage);
    auto *progressRow = new QHBoxLayout;
    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    progressRow->addWidget(m_progress, 1);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setVisible(false);
    progressRow->addWidget(m_cancel);
    layout->addLayout(progressRow);

    layout->addSpacing(8);
    auto *buttons = new QHBoxLayout;
    m_choose = new QPushButton(tr("I already have ffmpeg – choose ffmpeg.exe…"), this);
    buttons->addWidget(m_choose);
    buttons->addStretch();
    m_later = new QPushButton(tr("Later"), this);
    buttons->addWidget(m_later);
    layout->addLayout(buttons);

    connect(m_download, &QPushButton::clicked, this, &ToolsDialog::startDownload);
    connect(m_choose, &QPushButton::clicked, this, &ToolsDialog::choose);
    connect(m_later, &QPushButton::clicked, this, &ToolsDialog::reject);
    connect(m_cancel, &QPushButton::clicked, m_installer, &FfmpegInstaller::cancel);
    connect(m_installer, &FfmpegInstaller::stageChanged, m_stage, &QLabel::setText);
    connect(m_installer, &FfmpegInstaller::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(int(received * 1000 / total));
            m_progress->setFormat(QStringLiteral("%1 / %2 MB").arg(received >> 20).arg(total >> 20));
        }
    });
    connect(m_installer, &FfmpegInstaller::finished, this, &ToolsDialog::onFinished);
}

void ToolsDialog::startDownload()
{
    if (m_installer->isRunning())
        return;
    setBusy(true);
    m_stage->setStyleSheet({});
    m_progress->setRange(0, 0); // busy until the size is known
    m_progress->setFormat(QString());
    m_installer->start();
}

void ToolsDialog::setBusy(bool busy)
{
    m_download->setEnabled(!busy);
    m_choose->setEnabled(!busy);
    m_later->setEnabled(!busy);
    m_stage->setVisible(true);
    m_progress->setVisible(busy);
    m_cancel->setVisible(busy);
}

void ToolsDialog::onFinished(const QString &error)
{
    setBusy(false);
    if (error.isEmpty()) {
        m_outcome = Outcome::Installed;
        accept();
        return;
    }
    m_stage->setStyleSheet(QStringLiteral("color:#d32f2f"));
    m_stage->setText(error);
}

void ToolsDialog::choose()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose ffmpeg.exe"), QString(),
                                                      tr("ffmpeg (ffmpeg.exe);;Programs (*.exe)"));
    if (file.isEmpty())
        return;
    if (!QFileInfo::exists(QFileInfo(file).absolutePath() + QStringLiteral("/ffprobe.exe"))) {
        QMessageBox::warning(this, windowTitle(),
                             tr("ffprobe.exe must be in the same folder as ffmpeg.exe. Every ffmpeg download "
                                "contains both."));
        return;
    }
    m_chosen = file;
    m_outcome = Outcome::Chosen;
    accept();
}

void ToolsDialog::reject()
{
    if (m_installer->isRunning())
        m_installer->cancel(); // closing the window cancels the download
    QDialog::reject();
}
