#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QObject;

namespace tt {

struct Settings;

/// A usable ffmpeg/ffprobe pair, or the reason why none was found.
struct ToolLocation
{
    QString ffmpeg;   ///< absolute path of ffmpeg.exe
    QString ffprobe;  ///< absolute path of ffprobe.exe
    QString version;  ///< first line of `ffmpeg -version`
    QString origin;   ///< where it was found, for display ("downloaded by TanzTrans", "Windows PATH", ...)
    QString error;    ///< empty if the pair is usable

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

/// Folder the one-click download installs into: %LOCALAPPDATA%\TanzTrans\ffmpeg.
QString managedToolsDir();

/// True if the settings leave both tool paths on "automatic" (empty, or the bare names
/// "ffmpeg"/"ffprobe" that older versions stored).
bool toolsAutomatic(const Settings &settings);

/// Finds and tests ffmpeg + ffprobe. Blocking (starts both programs), so call it off the
/// GUI thread – see locateToolsAsync(). Paths set in the settings are used as they are
/// (an empty ffprobe path means "next to ffmpeg"). On "automatic" the first folder that
/// holds a working pair wins, in this order: next to TanzTrans.exe, managedToolsDir(),
/// the Windows PATH (read fresh from the registry, so a new install is found without a
/// restart), the winget link folders. A working ffmpeg needs the libx265 encoder.
ToolLocation locateTools(const Settings &settings);

/// Runs locateTools() on a worker thread and calls `done` on `context`'s thread
/// (not at all if `context` is gone by then).
void locateToolsAsync(const Settings &settings, QObject *context, std::function<void(ToolLocation)> done);

} // namespace tt
