#include "HiDriveClient.h"

#include "Log.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace tt {

namespace {
const QByteArray kApiBase = QByteArrayLiteral("https://api.hidrive.strato.com/2.1");
constexpr qint64 kChunkSize = 64LL * 1024 * 1024;
constexpr char kUserAbortProperty[] = "tt_userAbort"; // set on replies cancelled by abortTransfers()
const QString kEntryFields = QStringLiteral("name,path,id,type,size,mtime,chash,mime_type");
const QString kMemberFields = QStringLiteral(
    "path,members.name,members.path,members.id,members.type,members.size,members.mtime,members.chash,members.mime_type");

QString decode(const QJsonValue &value)
{
    return QUrl::fromPercentEncoding(value.toString().toUtf8());
}
} // namespace

struct HiDriveClient::ListState
{
    QList<RemoteEntry> files;
    int pending = 0;
    QString error;
    std::function<void(Result<QList<RemoteEntry>>)> done;
};

struct HiDriveClient::UploadState
{
    std::unique_ptr<QFile> file;
    QString dir;
    QString name;
    QString path;
    qint64 total = 0;
    qint64 offset = 0;
    Progress progress;
    std::function<void(Result<RemoteEntry>)> done;
};

HiDriveClient::HiDriveClient(QObject *parent)
    : QObject(parent)
{
}

void HiDriveClient::configure(const ShareLink &link, const QString &password, bool writeAllowed)
{
    if (link.shareId != m_link.shareId || password != m_password) {
        m_token.clear();
        m_tokenExpiry = {};
    }
    m_link = link;
    m_password = password;
    m_writeAllowed = writeAllowed;
    Log::instance().addSecret(link.shareId);
    Log::instance().addSecret(password);
}

QUrl HiDriveClient::apiUrl(const QString &endpoint, const QueryList &query) const
{
    QByteArray url = kApiBase + endpoint.toUtf8();
    bool first = true;
    for (const auto &[key, value] : query) {
        url += first ? '?' : '&';
        first = false;
        url += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    return QUrl::fromEncoded(url, QUrl::StrictMode);
}

QString HiDriveClient::fileUrl(const QString &path) const
{
    return apiUrl(QStringLiteral("/file"), {{QStringLiteral("path"), path}}).toString(QUrl::FullyEncoded);
}

void HiDriveClient::ensureToken(std::function<void(Result<QString>)> done)
{
    if (!isConfigured()) {
        done(fail(QStringLiteral("no valid HiDrive share link configured")));
        return;
    }
    if (!m_token.isEmpty() && QDateTime::currentDateTimeUtc().secsTo(m_tokenExpiry) > 300) {
        done(m_token);
        return;
    }
    m_tokenWaiters.append(std::move(done));
    if (m_tokenPending)
        return;
    m_tokenPending = true;

    QNetworkRequest request(apiUrl(QStringLiteral("/share/token")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(60000);
    QByteArray body = "id=" + QUrl::toPercentEncoding(m_link.shareId);
    if (!m_password.isEmpty())
        body += "&password=" + QUrl::toPercentEncoding(m_password);

    QNetworkReply *reply = m_nam.post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_tokenPending = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        Result<QString> result = fail(QString());
        if (status == 200) {
            const QJsonObject o = QJsonDocument::fromJson(data).object();
            m_token = o.value(QLatin1String("access_token")).toString();
            const int expires = o.value(QLatin1String("expires_in")).toInt(3600);
            m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(expires);
            Log::instance().addSecret(m_token);
            result = m_token.isEmpty() ? Result<QString>(fail(QStringLiteral("token response without access_token")))
                                       : Result<QString>(m_token);
        } else {
            QString text = errorText(status, data, reply->error() != QNetworkReply::NoError ? reply->errorString() : QString());
            if (status == 401)
                text = QStringLiteral("share requires a password");
            else if (status == 403)
                text = QStringLiteral("wrong share password or access denied");
            else if (status == 404)
                text = QStringLiteral("share not found (check the link)");
            else if (status == 410)
                text = QStringLiteral("share link has expired");
            result = fail(QStringLiteral("HiDrive login failed: %1").arg(text));
        }
        const auto waiters = std::exchange(m_tokenWaiters, {});
        for (const auto &waiter : waiters)
            waiter(result);
    });
}

void HiDriveClient::call(const QByteArray &verb, const QUrl &url, const QByteArray &body, ReplyHandler handler,
                         std::function<void(QNetworkReply *)> started, bool isTransfer, bool retried)
{
    ensureToken([this, verb, url, body, handler = std::move(handler), started = std::move(started), isTransfer,
                 retried](Result<QString> token) {
        if (!token) {
            handler(0, {}, token.error());
            return;
        }
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Bearer " + token->toUtf8());
        if (!body.isEmpty() || verb == "PUT" || verb == "PATCH" || verb == "POST")
            request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));
        request.setTransferTimeout(isTransfer ? 120000 : 60000);
        QNetworkReply *reply = body.isEmpty() && verb == "GET" ? m_nam.get(request)
                                                               : m_nam.sendCustomRequest(request, verb, body);
        if (isTransfer)
            m_transfers.append(reply);
        if (started)
            started(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, verb, url, body, handler, started, isTransfer, retried] {
            reply->deleteLater();
            m_transfers.removeAll(reply);
            if (reply->property(kUserAbortProperty).toBool()) {
                handler(0, {}, QString(kAbortedError));
                return;
            }
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 401 && !retried) { // token revoked or expired early: get a new one
                m_token.clear();
                call(verb, url, body, handler, started, isTransfer, true);
                return;
            }
            const QByteArray data = reply->readAll();
            QString networkError;
            if (reply->error() == QNetworkReply::OperationCanceledError)
                networkError = QStringLiteral("no response from HiDrive (timeout)");
            else if (reply->error() != QNetworkReply::NoError && status == 0)
                networkError = reply->errorString();
            handler(status, data, networkError);
        });
    });
}

Result<void> HiDriveClient::checkWrite(const QString &path) const
{
    if (!m_writeAllowed)
        return fail(QStringLiteral("write refused: read-only mode is active"));
    if (!isInsideBase(m_link.basePath, path))
        return fail(QStringLiteral("write refused: %1 is outside the monitored folder %2").arg(path, m_link.basePath));
    return {};
}

QString HiDriveClient::errorText(int status, const QByteArray &body, const QString &networkError)
{
    if (!networkError.isEmpty())
        return networkError;
    const QString msg = QJsonDocument::fromJson(body).object().value(QLatin1String("msg")).toString();
    return msg.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : QStringLiteral("HTTP %1: %2").arg(status).arg(msg);
}

RemoteEntry HiDriveClient::parseEntry(const QJsonObject &o)
{
    RemoteEntry e;
    e.name = decode(o.value(QLatin1String("name")));
    e.path = decode(o.value(QLatin1String("path")));
    e.id = o.value(QLatin1String("id")).toString();
    e.chash = o.value(QLatin1String("chash")).toString();
    e.mime = o.value(QLatin1String("mime_type")).toString();
    e.size = qint64(o.value(QLatin1String("size")).toDouble());
    e.mtime = qint64(o.value(QLatin1String("mtime")).toDouble());
    e.isDir = o.value(QLatin1String("type")).toString() == QLatin1String("dir");
    return e;
}

void HiDriveClient::listDir(const QString &path, std::function<void(Result<QList<RemoteEntry>>)> done)
{
    const QUrl url = apiUrl(QStringLiteral("/dir"), {{QStringLiteral("path"), path},
                                                     {QStringLiteral("members"), QStringLiteral("all")},
                                                     {QStringLiteral("fields"), kMemberFields},
                                                     {QStringLiteral("limit"), QStringLiteral("0,10000")}});
    call("GET", url, {}, [done = std::move(done)](int status, const QByteArray &body, const QString &err) {
        if (status != 200) {
            done(fail(errorText(status, body, err)));
            return;
        }
        QList<RemoteEntry> entries;
        const QJsonArray members = QJsonDocument::fromJson(body).object().value(QLatin1String("members")).toArray();
        for (const QJsonValue &m : members)
            entries.append(parseEntry(m.toObject()));
        done(entries);
    });
}

void HiDriveClient::listRecursive(const QString &root, std::function<void(Result<QList<RemoteEntry>>)> done)
{
    auto state = std::make_shared<ListState>();
    state->done = std::move(done);
    listInto(state, root);
}

void HiDriveClient::listInto(const std::shared_ptr<ListState> &state, const QString &path)
{
    ++state->pending;
    listDir(path, [this, state, path](Result<QList<RemoteEntry>> result) {
        if (!result) {
            if (state->error.isEmpty())
                state->error = QStringLiteral("%1: %2").arg(path, result.error());
        } else if (state->error.isEmpty()) {
            for (const RemoteEntry &entry : *result) {
                if (entry.isDir)
                    listInto(state, entry.path);
                else
                    state->files.append(entry);
            }
        }
        if (--state->pending == 0) {
            if (state->error.isEmpty())
                state->done(state->files);
            else
                state->done(fail(state->error));
        }
    });
}

void HiDriveClient::meta(const QString &path, std::function<void(Result<std::optional<RemoteEntry>>)> done)
{
    const QUrl url = apiUrl(QStringLiteral("/meta"), {{QStringLiteral("path"), path},
                                                      {QStringLiteral("fields"), kEntryFields}});
    call("GET", url, {}, [done = std::move(done)](int status, const QByteArray &body, const QString &err) {
        if (status == 404) {
            done(std::optional<RemoteEntry>());
            return;
        }
        if (status != 200) {
            done(fail(errorText(status, body, err)));
            return;
        }
        done(std::optional<RemoteEntry>(parseEntry(QJsonDocument::fromJson(body).object())));
    });
}

void HiDriveClient::download(const QString &remotePath, const QString &localPath, Progress progress,
                             std::function<void(Result<void>)> done)
{
    const QString partPath = localPath + QStringLiteral(".part");
    auto file = std::make_shared<QFile>(partPath);
    const qint64 offset = file->exists() ? file->size() : 0;
    if (!file->open(QIODevice::ReadWrite | QIODevice::Append)) {
        done(fail(QStringLiteral("cannot write %1: %2").arg(partPath, file->errorString())));
        return;
    }
    auto base = std::make_shared<qint64>(offset);
    auto writeError = std::make_shared<QString>();

    ensureToken([=, this](Result<QString> token) {
        if (!token) {
            done(fail(token.error()));
            return;
        }
        QNetworkRequest request(apiUrl(QStringLiteral("/file"), {{QStringLiteral("path"), remotePath}}));
        request.setRawHeader("Authorization", "Bearer " + token->toUtf8());
        if (offset > 0)
            request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
        request.setTransferTimeout(120000);
        QNetworkReply *reply = m_nam.get(request);
        m_transfers.append(reply);

        auto checkedStatus = std::make_shared<bool>(false);
        connect(reply, &QNetworkReply::readyRead, this, [=] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (!*checkedStatus) {
                *checkedStatus = true;
                if (status == 200 && *base > 0) { // server ignored the range: start over
                    file->resize(0);
                    file->seek(0);
                    *base = 0;
                }
            }
            if (status != 200 && status != 206)
                return;
            const QByteArray chunk = reply->readAll();
            if (file->write(chunk) != chunk.size() && writeError->isEmpty()) {
                *writeError = file->errorString();
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::downloadProgress, this, [=](qint64 received, qint64 total) {
            if (progress && total > 0)
                progress(*base + received, *base + total);
        });
        connect(reply, &QNetworkReply::finished, this, [=, this] {
            reply->deleteLater();
            m_transfers.removeAll(reply);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && (status == 200 || status == 206))
                file->write(reply->readAll());
            file->close();
            if (!writeError->isEmpty()) {
                done(fail(QStringLiteral("cannot write %1: %2").arg(partPath, *writeError)));
                return;
            }
            if (reply->property(kUserAbortProperty).toBool()) {
                done(fail(QString(kAbortedError))); // the .part file stays for resuming
                return;
            }
            if (reply->error() != QNetworkReply::NoError || (status != 200 && status != 206)) {
                if (status == 416 || status == 404)
                    QFile::remove(partPath);
                if (status == 401)
                    m_token.clear();
                const QString err = reply->error() == QNetworkReply::OperationCanceledError
                                        ? QStringLiteral("no response from HiDrive (timeout)")
                                        : reply->errorString();
                // network-level error or timeout (also after a 206 header): Qt's text; else HiDrive's HTTP error
                const bool network = status == 0 || reply->error() == QNetworkReply::OperationCanceledError;
                done(fail(errorText(status, reply->readAll(), network ? err : QString())));
                return;
            }
            QFile::remove(localPath);
            if (!QFile::rename(partPath, localPath)) {
                done(fail(QStringLiteral("cannot rename %1").arg(partPath)));
                return;
            }
            done({});
        });
    });
}

void HiDriveClient::upload(const QString &localPath, const QString &remoteDir, const QString &name, Progress progress,
                           std::function<void(Result<RemoteEntry>)> done)
{
    const QString path = cleanRemotePath(remoteDir + QLatin1Char('/') + name);
    if (auto ok = checkWrite(path); !ok) {
        logError(ok.error());
        done(fail(ok.error()));
        return;
    }
    auto state = std::make_shared<UploadState>();
    state->file = std::make_unique<QFile>(localPath);
    if (!state->file->open(QIODevice::ReadOnly)) {
        done(fail(QStringLiteral("cannot read %1: %2").arg(localPath, state->file->errorString())));
        return;
    }
    state->dir = cleanRemotePath(remoteDir);
    state->name = name;
    state->path = path;
    state->total = state->file->size();
    state->progress = std::move(progress);
    state->done = std::move(done);
    sendChunk(state);
}

void HiDriveClient::sendChunk(const std::shared_ptr<UploadState> &state)
{
    state->file->seek(state->offset);
    const QByteArray chunk = state->file->read(kChunkSize);
    QUrl url;
    QByteArray verb;
    if (state->offset == 0) {
        verb = "PUT"; // creates or overwrites
        url = apiUrl(QStringLiteral("/file"), {{QStringLiteral("dir"), state->dir}, {QStringLiteral("name"), state->name}});
    } else {
        verb = "PATCH";
        url = apiUrl(QStringLiteral("/file"), {{QStringLiteral("path"), state->path},
                                               {QStringLiteral("offset"), QString::number(state->offset)}});
    }
    const qint64 chunkStart = state->offset;
    call(
        verb, url, chunk,
        [this, state, chunkSize = chunk.size()](int status, const QByteArray &body, const QString &err) {
            if (err == kAbortedError) {
                state->done(fail(QString(kAbortedError)));
                return;
            }
            if (status != 200 && status != 201 && status != 204) {
                state->done(fail(QStringLiteral("upload of %1 failed: %2").arg(state->name, errorText(status, body, err))));
                return;
            }
            state->offset += chunkSize;
            if (state->progress)
                state->progress(state->offset, state->total);
            if (state->offset < state->total) {
                sendChunk(state);
                return;
            }
            meta(state->path, [state](Result<std::optional<RemoteEntry>> m) {
                if (!m) {
                    state->done(fail(m.error()));
                } else if (!m->has_value()) {
                    state->done(fail(QStringLiteral("uploaded file %1 not found").arg(state->path)));
                } else if ((*m)->size != state->total) {
                    state->done(fail(QStringLiteral("uploaded size %1 differs from local size %2")
                                         .arg((*m)->size)
                                         .arg(state->total)));
                } else {
                    state->done(**m);
                }
            });
        },
        [state, chunkStart](QNetworkReply *reply) {
            QObject::connect(reply, &QNetworkReply::uploadProgress, reply, [state, chunkStart](qint64 sent, qint64) {
                if (state->progress)
                    state->progress(chunkStart + sent, state->total);
            });
        },
        true);
}

void HiDriveClient::rename(const QString &path, const QString &newName, std::function<void(Result<RemoteEntry>)> done)
{
    const QString target = remoteParent(path) + QLatin1Char('/') + newName;
    for (const QString &p : {path, target}) {
        if (auto ok = checkWrite(p); !ok) {
            logError(ok.error());
            done(fail(ok.error()));
            return;
        }
    }
    const QUrl url = apiUrl(QStringLiteral("/file/rename"), {{QStringLiteral("path"), path}, {QStringLiteral("name"), newName}});
    call("POST", url, {}, [done = std::move(done)](int status, const QByteArray &body, const QString &err) {
        if (status != 200) {
            done(fail(errorText(status, body, err)));
            return;
        }
        done(parseEntry(QJsonDocument::fromJson(body).object()));
    });
}

void HiDriveClient::removeFile(const QString &path, std::function<void(Result<void>)> done)
{
    if (auto ok = checkWrite(path); !ok) {
        logError(ok.error());
        done(fail(ok.error()));
        return;
    }
    const QUrl url = apiUrl(QStringLiteral("/file"), {{QStringLiteral("path"), path}});
    call("DELETE", url, {}, [done = std::move(done)](int status, const QByteArray &body, const QString &err) {
        if (status != 204 && status != 200) {
            done(fail(errorText(status, body, err)));
            return;
        }
        done({});
    });
}

void HiDriveClient::setMtime(const QString &path, qint64 mtime, std::function<void(Result<void>)> done)
{
    if (auto ok = checkWrite(path); !ok) {
        logError(ok.error());
        done(fail(ok.error()));
        return;
    }
    const QUrl url = apiUrl(QStringLiteral("/meta"), {{QStringLiteral("path"), path}, {QStringLiteral("mtime"), QString::number(mtime)}});
    call("PATCH", url, {}, [done = std::move(done)](int status, const QByteArray &body, const QString &err) {
        if (status != 204 && status != 200) {
            done(fail(errorText(status, body, err)));
            return;
        }
        done({});
    });
}

void HiDriveClient::abortTransfers()
{
    const auto transfers = m_transfers;
    for (const QPointer<QNetworkReply> &reply : transfers) {
        if (reply) {
            reply->setProperty(kUserAbortProperty, true);
            reply->abort();
        }
    }
}

} // namespace tt
