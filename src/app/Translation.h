#pragma once

#include <QString>

namespace tt::app {

/// Installs the translators for "en" (none) or "de". Sends QEvent::LanguageChange to all widgets.
void applyLanguage(const QString &language);

} // namespace tt::app
