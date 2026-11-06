#include "WinUtil.h"

#include <QDir>
#include <QSettings>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#  include <dpapi.h>
#endif

namespace tt::win {

namespace {
constexpr QLatin1StringView kPrefix("dpapi:");
}

QString protectString(const QString &plain)
{
    if (plain.isEmpty())
        return {};
#ifdef Q_OS_WIN
    QByteArray in = plain.toUtf8();
    DATA_BLOB input{DWORD(in.size()), reinterpret_cast<BYTE *>(in.data())};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"TanzTrans", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    const QByteArray encrypted(reinterpret_cast<const char *>(output.pbData), qsizetype(output.cbData));
    LocalFree(output.pbData);
    return kPrefix + QString::fromLatin1(encrypted.toBase64());
#else
    return plain;
#endif
}

QString unprotectString(const QString &stored)
{
    if (!stored.startsWith(kPrefix))
        return stored;
#ifdef Q_OS_WIN
    QByteArray in = QByteArray::fromBase64(stored.mid(kPrefix.size()).toLatin1());
    DATA_BLOB input{DWORD(in.size()), reinterpret_cast<BYTE *>(in.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData), qsizetype(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plain;
#else
    return {};
#endif
}

#ifdef Q_OS_WIN
namespace {
using NtProcessFn = LONG(NTAPI *)(HANDLE);

bool callNt(const char *name, qint64 pid)
{
    static HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = reinterpret_cast<NtProcessFn>(reinterpret_cast<void *>(GetProcAddress(ntdll, name)));
    if (!fn)
        return false;
    HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, DWORD(pid));
    if (!process)
        return false;
    const LONG status = fn(process);
    CloseHandle(process);
    return status >= 0;
}
} // namespace
#endif

bool suspendProcess(qint64 pid)
{
#ifdef Q_OS_WIN
    return callNt("NtSuspendProcess", pid);
#else
    Q_UNUSED(pid);
    return false;
#endif
}

bool resumeProcess(qint64 pid)
{
#ifdef Q_OS_WIN
    return callNt("NtResumeProcess", pid);
#else
    Q_UNUSED(pid);
    return false;
#endif
}

bool killWithThisProcess(qint64 pid)
{
#ifdef Q_OS_WIN
    // One job for the whole application; its handle is closed by Windows when we exit.
    static HANDLE job = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
                CloseHandle(h);
                h = nullptr;
            }
        }
        return h;
    }();
    if (!job)
        return false;
    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!process)
        return false;
    const bool ok = AssignProcessToJobObject(job, process);
    CloseHandle(process);
    return ok;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

void setAutostart(bool enabled, const QString &exePath, const QStringList &arguments)
{
#ifdef Q_OS_WIN
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    if (enabled) {
        QString command = QLatin1Char('"') + QDir::toNativeSeparators(exePath) + QLatin1Char('"');
        for (const QString &arg : arguments)
            command += QLatin1Char(' ') + arg;
        run.setValue(QStringLiteral("TanzTrans"), command);
    } else {
        run.remove(QStringLiteral("TanzTrans"));
    }
#else
    Q_UNUSED(enabled);
    Q_UNUSED(exePath);
    Q_UNUSED(arguments);
#endif
}

} // namespace tt::win
