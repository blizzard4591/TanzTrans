#include "Translation.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace tt::app {

void applyLanguage(const QString &language)
{
    static QTranslator *appTranslator = nullptr;
    static QTranslator *qtTranslator = nullptr;
    for (QTranslator **t : {&appTranslator, &qtTranslator}) {
        if (*t) {
            QCoreApplication::removeTranslator(*t);
            delete *t;
            *t = nullptr;
        }
    }
    if (language == QLatin1String("de")) {
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        appTranslator = new QTranslator;
        if (appTranslator->load(QStringLiteral(":/i18n/tanztrans_de.qm")))
            QCoreApplication::installTranslator(appTranslator);
        qtTranslator = new QTranslator;
        const QString deployed = QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
        if (qtTranslator->load(QStringLiteral("qtbase_de"), deployed)
            || qtTranslator->load(QStringLiteral("qtbase_de"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
            QCoreApplication::installTranslator(qtTranslator);
    } else {
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    }
}

} // namespace tt::app
