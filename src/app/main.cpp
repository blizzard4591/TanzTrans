#include "AppIcon.h"
#include "JobManager.h"
#include "Log.h"
#include "MainWindow.h"
#include "Translation.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("TanzTrans"));
    QApplication::setApplicationVersion(QStringLiteral(TT_VERSION));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(tt::app::makeAppIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Transcodes oversized videos in a HiDrive share to HEVC."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption configOption(QStringLiteral("config"), QStringLiteral("Settings file (INI) to use."),
                                          QStringLiteral("file"));
    const QCommandLineOption linkFileOption(QStringLiteral("link-file"),
                                            QStringLiteral("Read the share link from this file (overrides the settings)."),
                                            QStringLiteral("file"));
    const QCommandLineOption minimizedOption(QStringLiteral("minimized"), QStringLiteral("Start in the tray."));
    const QCommandLineOption pausedOption(QStringLiteral("paused"),
                                          QStringLiteral("Start paused (scan and check only, no downloads or encodes)."));
    const QCommandLineOption exitIdleOption(QStringLiteral("exit-when-idle"),
                                            QStringLiteral("Quit when there is nothing left to do (testing)."));
    const QCommandLineOption tabOption(QStringLiteral("tab"), QStringLiteral("Open on this tab: queue, log or settings."),
                                       QStringLiteral("name"));
    parser.addOptions({configOption, linkFileOption, minimizedOption, pausedOption, exitIdleOption, tabOption});
    parser.process(app);

    const QString iniPath = parser.isSet(configOption)
                                ? QFileInfo(parser.value(configOption)).absoluteFilePath()
                                : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                                      + QStringLiteral("/TanzTrans.ini");
    tt::Settings settings;
    {
        QSettings ini(iniPath, QSettings::IniFormat);
        settings = tt::Settings::load(ini);
    }
    if (parser.isSet(linkFileOption))
        settings.linkFile = QFileInfo(parser.value(linkFileOption)).absoluteFilePath();

    QDir().mkpath(QFileInfo(settings.dbPath).absolutePath());
    QLockFile lock(settings.dbPath + QStringLiteral(".lock"));
    if (!lock.tryLock(200)) {
        QMessageBox::information(nullptr, QStringLiteral("TanzTrans"),
                                 QObject::tr("TanzTrans is already running with this database."));
        return 0;
    }

    tt::Log::instance().setLogFile(settings.logDir + QStringLiteral("/tanztrans.log"));
    tt::app::applyLanguage(settings.language);
    tt::logInfo(QObject::tr("TanzTrans %1 started, settings: %2").arg(QStringLiteral(TT_VERSION), QDir::toNativeSeparators(iniPath)));

    tt::JobManager jobs;
    if (auto ok = jobs.initialize(settings); !ok) {
        QMessageBox::critical(nullptr, QStringLiteral("TanzTrans"), ok.error());
        return 1;
    }
    const bool paused = parser.isSet(pausedOption);
    if (paused)
        jobs.pause();

    QStringList autostartArgs{QStringLiteral("--minimized")};
    if (parser.isSet(configOption))
        autostartArgs << QStringLiteral("--config") << QLatin1Char('"') + QDir::toNativeSeparators(iniPath) + QLatin1Char('"');

    MainWindow window(&jobs, iniPath, autostartArgs);
    if (parser.isSet(linkFileOption))
        window.setLinkFileOverride(settings.linkFile);
    const QStringList tabs{QStringLiteral("queue"), QStringLiteral("log"), QStringLiteral("settings")};
    if (parser.isSet(tabOption))
        window.setCurrentTab(std::max<qsizetype>(0, tabs.indexOf(parser.value(tabOption).toLower())));
    else if (!jobs.linkError().isEmpty())
        window.setCurrentTab(2); // first start: the link has to be entered in the settings
    if (!parser.isSet(minimizedOption))
        window.show();

    if (parser.isSet(exitIdleOption)) {
        auto *idle = new QTimer(&app);
        QObject::connect(idle, &QTimer::timeout, &app, [&jobs, paused] {
            const bool done = paused ? jobs.isCheckIdle() : jobs.isIdle();
            if (done) {
                tt::logInfo(QStringLiteral("Nothing left to do – exiting (--exit-when-idle)."));
                QApplication::quit();
            }
        });
        idle->start(2000);
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &jobs, &tt::JobManager::shutdown);
    return app.exec();
}
