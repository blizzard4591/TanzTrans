#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace tt {
class FfmpegInstaller;
}

/// Shown when no usable ffmpeg is found: one-click download, or pick an existing ffmpeg.exe.
class ToolsDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Outcome { Later, Installed, Chosen };

    explicit ToolsDialog(const QString &reason, QWidget *parent = nullptr);

    void startDownload();
    [[nodiscard]] Outcome outcome() const { return m_outcome; }
    /// ffmpeg.exe picked by the user (Outcome::Chosen).
    [[nodiscard]] QString chosenFfmpeg() const { return m_chosen; }

    void reject() override;

private:
    void choose();
    void onFinished(const QString &error);
    void setBusy(bool busy);

    tt::FfmpegInstaller *m_installer = nullptr;
    QPushButton *m_download = nullptr;
    QPushButton *m_choose = nullptr;
    QPushButton *m_later = nullptr;
    QPushButton *m_cancel = nullptr;
    QLabel *m_stage = nullptr;
    QProgressBar *m_progress = nullptr;
    Outcome m_outcome = Outcome::Later;
    QString m_chosen;
};
