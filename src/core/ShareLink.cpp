#include "ShareLink.h"

#include <QDir>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

namespace tt {

std::optional<ShareLink> parseShareLink(const QString &link)
{
    static const QRegularExpression re(QStringLiteral(R"(/share/([A-Za-z0-9._~-]+)(?:#\$?(/[^?#]*))?)"));
    const auto match = re.match(link.trimmed());
    if (!match.hasMatch())
        return std::nullopt;

    ShareLink result;
    result.shareId = match.captured(1);
    QString base = match.captured(2);
    if (!base.isEmpty())
        base = QUrl::fromPercentEncoding(base.toUtf8());
    result.basePath = cleanRemotePath(base.isEmpty() ? QStringLiteral("/") : base);
    if (result.basePath.isEmpty())
        return std::nullopt;
    return result;
}

QString cleanRemotePath(const QString &path)
{
    QStringList parts;
    for (const QString &segment : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (segment == QLatin1String("."))
            continue;
        if (segment == QLatin1String(".."))
            return {};
        parts << segment;
    }
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

bool isInsideBase(const QString &base, const QString &path)
{
    const QString b = cleanRemotePath(base);
    const QString p = cleanRemotePath(path);
    if (b.isEmpty() || p.isEmpty())
        return false;
    if (b == QLatin1String("/"))
        return true;
    return p == b || p.startsWith(b + QLatin1Char('/'));
}

QString remoteParent(const QString &path)
{
    const QString p = cleanRemotePath(path);
    const qsizetype slash = p.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : p.left(slash);
}

QString remoteName(const QString &path)
{
    const QString p = cleanRemotePath(path);
    return p.mid(p.lastIndexOf(QLatin1Char('/')) + 1);
}

QString relativeToBase(const QString &base, const QString &path)
{
    const QString b = cleanRemotePath(base);
    const QString p = cleanRemotePath(path);
    if (!isInsideBase(b, p) || p == b)
        return {};
    return b == QLatin1String("/") ? p.mid(1) : p.mid(b.size() + 1);
}

namespace {
QString localSegment(QString segment)
{
    static const QString forbidden = QStringLiteral("<>:\"\\|?*");
    for (QChar &c : segment) {
        if (c.unicode() < 32 || forbidden.contains(c))
            c = QLatin1Char('_');
    }
    while (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' ')))
        segment.chop(1); // Windows drops them, which would turn ".." into a parent reference
    if (segment.isEmpty())
        segment = QStringLiteral("_");
    static const QRegularExpression device(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\..*)?$)"),
                                           QRegularExpression::CaseInsensitiveOption);
    if (device.match(segment).hasMatch())
        segment.prepend(QLatin1Char('_'));
    return segment;
}
} // namespace

QString localMirrorPath(const QString &localRoot, const QString &relativePath)
{
    QStringList parts;
    for (const QString &segment : relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        parts << localSegment(segment);
    if (parts.isEmpty())
        return {};
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(localRoot));
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    const QString result = QDir::cleanPath(prefix + parts.join(QLatin1Char('/')));
    return result.startsWith(prefix, Qt::CaseInsensitive) ? result : QString();
}

} // namespace tt
