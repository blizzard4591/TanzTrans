#pragma once

#include <QString>
#include <QStringList>

namespace tt::win {

/// DPAPI (current user) encryption for secrets stored in the INI file.
/// Returns "dpapi:<base64>"; on non-Windows platforms the text is returned unchanged.
QString protectString(const QString &plain);
/// Reverses protectString(). Values without the "dpapi:" prefix are returned unchanged.
QString unprotectString(const QString &stored);

/// Freeze / continue all threads of a process (NtSuspendProcess / NtResumeProcess).
bool suspendProcess(qint64 pid);
bool resumeProcess(qint64 pid);

/// Puts the process into a job object that Windows kills when TanzTrans exits or crashes,
/// so no orphaned (or forever suspended) ffmpeg keeps running and locking files.
bool killWithThisProcess(qint64 pid);

/// Start with Windows via HKCU\...\Run.
void setAutostart(bool enabled, const QString &exePath, const QStringList &arguments);

} // namespace tt::win
