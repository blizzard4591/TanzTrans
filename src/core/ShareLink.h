#pragma once

#include <QString>

#include <optional>

namespace tt {

/// A HiDrive share link: https://my.hidrive.com/share/<id>#$/<folder>.
/// The id addresses the whole share; the folder in the fragment is the monitored base folder.
struct ShareLink
{
    QString shareId;
    QString basePath = QStringLiteral("/");
};

std::optional<ShareLink> parseShareLink(const QString &link);

/// "/a//b/" -> "/a/b". Returns an empty string if the path contains "..".
QString cleanRemotePath(const QString &path);

/// True if path equals base or lies below it (after cleaning).
bool isInsideBase(const QString &base, const QString &path);

QString remoteParent(const QString &path);
QString remoteName(const QString &path);
/// Path of `path` relative to `base` ("" if equal), without leading slash.
QString relativeToBase(const QString &base, const QString &path);

/// Local path below `localRoot` that mirrors the remote relative path "a/b/c.mp4". Each
/// segment becomes a valid Windows name (<>:"\|?* and control characters -> '_', trailing
/// dots and spaces removed, "." / ".." / device names like CON made harmless), so the result
/// never leaves `localRoot`. Empty if `relativePath` has no segments.
QString localMirrorPath(const QString &localRoot, const QString &relativePath);

} // namespace tt
