#pragma once

#include "Result.h"
#include "ShareLink.h"

#include <QDateTime>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>

class QNetworkReply;

namespace tt {

/// Exact error text of a transfer cancelled by abortTransfers() (Stop / shutdown).
/// Timeouts and network errors never produce this text.
inline constexpr QLatin1StringView kAbortedError{"aborted"};

struct RemoteEntry
{
    QString path;  ///< decoded
    QString name;  ///< decoded
    QString id;
    QString chash;
    QString mime;
    qint64 size = 0;
    qint64 mtime = 0;
    bool isDir = false;
};

/// HiDrive REST API 2.1 client that works with a share link only (no owner credentials).
/// All write operations are refused unless write access is enabled AND the target path is
/// inside the base folder of the share link.
class HiDriveClient : public QObject
{
    Q_OBJECT
public:
    using Progress = std::function<void(qint64 done, qint64 total)>;

    explicit HiDriveClient(QObject *parent = nullptr);

    void configure(const ShareLink &link, const QString &password, bool writeAllowed);
    [[nodiscard]] const ShareLink &link() const { return m_link; }
    [[nodiscard]] bool isConfigured() const { return !m_link.shareId.isEmpty(); }
    [[nodiscard]] bool writeAllowed() const { return m_writeAllowed; }
    void setWriteAllowed(bool allowed) { m_writeAllowed = allowed; }

    void ensureToken(std::function<void(Result<QString>)> done);
    [[nodiscard]] QString fileUrl(const QString &path) const;

    void listDir(const QString &path, std::function<void(Result<QList<RemoteEntry>>)> done);
    void listRecursive(const QString &root, std::function<void(Result<QList<RemoteEntry>>)> done);
    /// nullopt if the path does not exist.
    void meta(const QString &path, std::function<void(Result<std::optional<RemoteEntry>>)> done);

    /// Downloads to `localPath` via `localPath.part`; resumes an existing .part file.
    void download(const QString &remotePath, const QString &localPath, Progress progress,
                  std::function<void(Result<void>)> done);
    /// Uploads in chunks (PUT first chunk = create/overwrite, PATCH the rest).
    void upload(const QString &localPath, const QString &remoteDir, const QString &name, Progress progress,
                std::function<void(Result<RemoteEntry>)> done);
    void rename(const QString &path, const QString &newName, std::function<void(Result<RemoteEntry>)> done);
    void removeFile(const QString &path, std::function<void(Result<void>)> done);
    void setMtime(const QString &path, qint64 mtime, std::function<void(Result<void>)> done);

    /// Aborts running downloads/uploads; their callbacks receive kAbortedError.
    void abortTransfers();

private:
    using ReplyHandler = std::function<void(int httpStatus, const QByteArray &body, const QString &networkError)>;
    using QueryList = QList<std::pair<QString, QString>>;

    struct ListState;
    struct UploadState;

    [[nodiscard]] QUrl apiUrl(const QString &endpoint, const QueryList &query = {}) const;
    /// Sends an authorised request. A 401 drops the cached token and retries once.
    void call(const QByteArray &verb, const QUrl &url, const QByteArray &body, ReplyHandler handler,
              std::function<void(QNetworkReply *)> started = {}, bool isTransfer = false, bool retried = false);
    [[nodiscard]] Result<void> checkWrite(const QString &path) const;
    void listInto(const std::shared_ptr<ListState> &state, const QString &path);
    void sendChunk(const std::shared_ptr<UploadState> &state);

    static QString errorText(int status, const QByteArray &body, const QString &networkError);
    static RemoteEntry parseEntry(const QJsonObject &object);

    QNetworkAccessManager m_nam;
    ShareLink m_link;
    QString m_password;
    bool m_writeAllowed = false;
    QString m_token;
    QDateTime m_tokenExpiry;
    bool m_tokenPending = false;
    QList<std::function<void(Result<QString>)>> m_tokenWaiters;
    QList<QPointer<QNetworkReply>> m_transfers;
};

} // namespace tt
