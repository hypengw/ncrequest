module;
#include <QDateTime>
#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QHttpHeaders>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkCookieJar>
#include <QNetworkCookie>
#include <QSharedPointer>
#include <QList>
#include <memory>
#include <functional>
#include <algorithm>
#include <atomic>
module ncrequest;
import :client.qt_network.resource;
using namespace rstd::prelude;
using namespace rstd::literals;

namespace ncrequest::client::qt_network
{
static std::atomic<quint64>                          next_resource { 1 };
static QMutex                                        managers_mutex;
static QHash<QNetworkAccessManager*, ResourceCache*> managers;

struct ResourceRecord {
    QPointer<QNetworkAccessManager> manager;
    QString                         key;
    QUrl                            url;
    QString identity { QStringLiteral("ncrequest-resource:%1").arg(next_resource.fetch_add(1)) };
    lihttpto::CachePolicy policy;
    QDeadlineTimer        deadline { 0, Qt::PreciseTimer };
    bool                  allowed {}, ready {}, invalid {};
};
struct ResourceOperation {
    QPointer<ResourceCache>        owner;
    QNetworkRequest                request;
    QPointer<QNetworkReply>        reply;
    QSharedPointer<ResourceRecord> prior, selected;
    quint64                        epoch {};
    qint64                         requested {}, received {};
    bool                           started {}, done {}, discarded {}, reusing {};
    ~ResourceOperation() {
        if (reply) {
            if (! done) reply->abort();
            reply->deleteLater();
        }
    }
};

static auto origin(const QUrl& url) -> QUrl {
    auto result = url;
    result.setPath({});
    result.setQuery({});
    result.setFragment({});
    result.setPort(url.port(url.scheme() == QStringLiteral("https") ? 443 : 80));
    return result;
}
static auto key(const QNetworkRequest& request) -> QString {
    auto url = request.url();
    url.setFragment({});
    auto result = url.toString(QUrl::FullyEncoded);
    auto names  = request.rawHeaderList();
    std::sort(names.begin(), names.end());
    for (const auto& name : names)
        result += QStringLiteral("\n%1:%2:%3")
                      .arg(QString::fromLatin1(name.toLower()))
                      .arg(request.rawHeader(name).size())
                      .arg(QString::fromLatin1(request.rawHeader(name).toHex()));
    return result;
}
static auto parse_fields(const QHttpHeaders& headers) -> lihttpto::CacheFields {
    lihttpto::Headers fields;
    qsizetype         bytes {};
    for (const auto& [name, value] : headers.toListOfPairs()) {
        bytes += name.size() + value.size() + 4;
        auto text = rstd::str_::from_utf8(slice<u8>::from_raw_parts(
            reinterpret_cast<const byte*>(name.data()), usize(name.size())));
        if (text.is_err() || bytes > 65536) {
            lihttpto::CacheFields invalid;
            invalid.reusable = false;
            return invalid;
        }
        auto field = lihttpto::Header::make(
            *text,
            slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(value.data()),
                                      usize(value.size())));
        if (field.is_err()) {
            lihttpto::CacheFields invalid;
            invalid.reusable = false;
            return invalid;
        }
        fields.push(rstd::move(field).unwrap());
    }
    return lihttpto::CacheFields::parse(fields);
}
static auto strong_equal(const lihttpto::CachePolicy& a, const lihttpto::CachePolicy& b) -> bool {
    if (a.fields.etag.is_none() || b.fields.etag.is_none()) return false;
    auto left  = lihttpto::EntityTag::parse(a.fields.etag->as_str().as_bytes());
    auto right = lihttpto::EntityTag::parse(b.fields.etag->as_str().as_bytes());
    return left.is_ok() && right.is_ok() && left->strong_eq(*right);
}

ResourceSnapshot::ResourceSnapshot(QSharedPointer<ResourceRecord> record)
    : record_(std::move(record)) {}
auto ResourceSnapshot::identity() const -> QString {
    return record_ ? record_->identity : QString();
}
auto ResourceSnapshot::reusable() const -> bool {
    return record_ && record_->allowed && record_->ready && ! record_->invalid &&
           record_->manager && record_->manager->cookieJar()->cookiesForUrl(record_->url).isEmpty();
}
auto ResourceSnapshot::fresh() const -> bool {
    return reusable() && ! record_->policy.fields.control.no_cache &&
           ! record_->deadline.hasExpired();
}

class ResourceCache::Private {
public:
    struct Parsed {
        QList<QPair<QByteArray, QByteArray>> headers;
        lihttpto::CacheFields                fields;
    };
    struct Observer {
        QPointer<QObject>                receiver;
        std::function<void(const QUrl&)> callback;
    };
    ResourceCache*                                         owner;
    QPointer<QNetworkAccessManager>                        manager;
    QPointer<QNetworkDiskCache>                            disk;
    ResourceOptions                                        options;
    QHash<QString, QSharedPointer<ResourceRecord>>         entries;
    QHash<QNetworkReply*, QWeakPointer<ResourceOperation>> operations;
    QList<Observer>                                        observers;
    QList<QSharedPointer<Parsed>>                          parsed;
    quint64                                                epoch {};
    Private(ResourceCache* owner, QNetworkAccessManager* manager, ResourceOptions options)
        : owner(owner), manager(manager), options(std::move(options)) {}
    auto policy(const QHttpHeaders& headers, qint64 requested, qint64 received,
                bool* defaulted = nullptr) -> lihttpto::CachePolicy {
        QSharedPointer<Parsed> found;
        const auto             pairs = headers.toListOfPairs();
        for (const auto& candidate : parsed)
            if (candidate->headers == pairs) {
                found = candidate;
                break;
            }
        if (! found) {
            found = QSharedPointer<Parsed>::create(Parsed { pairs, parse_fields(headers) });
            parsed.append(found);
            while (parsed.size() > qMax<qsizetype>(1, options.maximum_entries))
                parsed.removeFirst();
        }
        auto fields   = found->fields.clone();
        bool fallback = fields.control.max_age.is_none() && fields.expires.is_none() &&
                        ! fields.control.invalid && ! fields.control.no_cache && fields.reusable &&
                        options.fallback_seconds > 0;
        if (fallback)
            fields.control.max_age =
                Some(i64(qMin<qint64>(2147483648ll, options.fallback_seconds)));
        if (defaulted) *defaulted = fallback;
        return { rstd::move(fields), i64(requested), i64(received), i64(received) };
    }
    bool permitted(const QNetworkRequest& request) const {
        if (! manager || ! disk || manager->cache() != disk) return false;
        if (! request.url().userInfo().isEmpty()) return false;
        if (request.hasRawHeader("Authorization") || request.hasRawHeader("Cookie") ||
            request.hasRawHeader("Range") || request.hasRawHeader("If-None-Match") ||
            request.hasRawHeader("If-Modified-Since") || request.hasRawHeader("If-Match") ||
            request.hasRawHeader("If-Unmodified-Since") || request.hasRawHeader("If-Range"))
            return false;
        if (manager && ! manager->cookieJar()->cookiesForUrl(request.url()).isEmpty()) return false;
        auto fields = parse_fields(request.headers());
        if (! fields.reusable || fields.control.no_cache || fields.control.max_age.is_some() ||
            fields.control.min_fresh.is_some())
            return false;
        if (! request.attribute(QNetworkRequest::CacheSaveControlAttribute, true).toBool())
            return false;
        auto load = request.attribute(QNetworkRequest::CacheLoadControlAttribute);
        if (load.isValid() && load.toInt() == QNetworkRequest::AlwaysNetwork) return false;
        for (const auto& allowed : options.origins)
            if (origin(request.url()) == origin(allowed)) return true;
        return false;
    }
    bool admitted(const QHttpHeaders& headers) const {
        for (const auto& name : options.excluded_response_headers)
            if (headers.contains(name)) return false;
        return true;
    }
    void trim() {
        while (entries.size() > qMax<qsizetype>(0, options.maximum_entries)) {
            auto it             = entries.begin();
            it.value()->invalid = true;
            entries.erase(it);
        }
    }
    void headers(const QSharedPointer<ResourceOperation>& op) {
        if (! op->reply || op->discarded || op->epoch != epoch) return;
        auto* reply = op->reply.data();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        if (op->selected) return;
        op->received      = QDateTime::currentSecsSinceEpoch();
        auto record       = QSharedPointer<ResourceRecord>::create();
        record->manager   = manager;
        record->key       = key(op->request);
        record->url       = op->request.url();
        record->policy    = policy(reply->headers(), op->requested, op->received);
        record->allowed   = permitted(op->request) && admitted(reply->headers()) &&
                            record->policy.fields.reusable && reply->url() == op->request.url();
        auto remaining    = record->policy.lifetime() - record->policy.age(i64(op->received));
        auto remaining_ms = qMax<qint64>(0, remaining.to_primitive()) * 1000;
        bool cached       = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
        if (cached && manager && manager->cache()) {
            auto meta = manager->cache()->metaData(reply->url());
            if (meta.expirationDate().isValid())
                remaining_ms = qMin(remaining_ms,
                                    qMax<qint64>(0,
                                                 meta.expirationDate().toMSecsSinceEpoch() -
                                                     QDateTime::currentMSecsSinceEpoch()));
        }
        record->deadline = QDeadlineTimer(remaining_ms, Qt::PreciseTimer);
        if (op->prior && ! op->prior->invalid && op->prior->ready && record->allowed &&
            (cached || strong_equal(record->policy, op->prior->policy))) {
            record->identity = op->prior->identity;
            record->ready    = true;
            op->reusing      = true;
        }
        auto prior = entries.value(record->key);
        if (prior && prior != record) prior->invalid = true;
        op->selected = record;
        if (record->allowed) entries.insert(record->key, record);
        trim();
    }
    auto open(const QSharedPointer<ResourceOperation>& op) -> QNetworkReply* {
        if (op->started) return op->reply;
        if (! manager || op->epoch != epoch || op->discarded) return nullptr;
        op->started   = true;
        op->requested = QDateTime::currentSecsSinceEpoch();
        auto request  = op->request;
        if (! permitted(request)) {
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                 QNetworkRequest::AlwaysNetwork);
            request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        }
        op->reply   = manager->get(request);
        auto* reply = op->reply.data();
        operations.insert(reply, op.toWeakRef());
        auto weak = op.toWeakRef();
        QObject::connect(reply, &QNetworkReply::metaDataChanged, owner, [this, weak] {
            if (auto value = weak.toStrongRef()) headers(value);
        });
        return reply;
    }
    void finished(QNetworkReply* reply) {
        auto it = operations.find(reply);
        if (it == operations.end()) {
            owner->invalidate(reply->url());
            if (reply->request().url() != reply->url()) owner->invalidate(reply->request().url());
            return;
        }
        auto op = it.value().toStrongRef();
        operations.erase(it);
        if (! op) return;
        op->done = true;
        headers(op);
        if (! op->selected) return;
        auto record = op->selected;
        if (op->discarded || op->epoch != epoch ||
            (! op->reusing && reply->error() != QNetworkReply::NoError) ||
            entries.value(record->key) != record) {
            record->invalid = true;
            return;
        }
        record->ready = true;
    }
};

class ResourceDiskCache final : public QNetworkDiskCache {
    QPointer<ResourceCache> owner_;

public:
    ResourceDiskCache(ResourceCache* owner, QObject* parent)
        : QNetworkDiskCache(parent), owner_(owner) {}
    auto prepare(const QNetworkCacheMetaData& original) -> QIODevice* override {
        if (! owner_) return nullptr;
        const auto& options = owner_->d_->options;
        for (const auto& name : options.excluded_response_headers)
            if (original.headers().contains(name)) return nullptr;
        auto metadata = original;
        bool defaulted {};
        auto parsed = owner_->d_->policy(metadata.headers(),
                                         QDateTime::currentSecsSinceEpoch(),
                                         QDateTime::currentSecsSinceEpoch(),
                                         &defaulted);
        if (parsed.fields.control.no_store || ! parsed.fields.reusable) return nullptr;
        if (defaulted)
            metadata.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(qMax<qint64>(
                0,
                (parsed.lifetime() - parsed.age(i64(QDateTime::currentSecsSinceEpoch())))
                    .to_primitive())));
        return QNetworkDiskCache::prepare(metadata);
    }
    void clear() override {
        if (owner_) owner_->invalidate();
        QNetworkDiskCache::clear();
    }
};

ResourceCache::ResourceCache(QNetworkAccessManager* manager, ResourceOptions options)
    : QObject(manager), d_(std::make_unique<Private>(this, manager, std::move(options))) {
    Q_ASSERT(manager && manager->thread() == QThread::currentThread());
    QMutexLocker lock(&managers_mutex);
    Q_ASSERT(! managers.contains(manager));
    managers.insert(manager, this);
    QObject::connect(manager, &QNetworkAccessManager::finished, this, [this](QNetworkReply* reply) {
        d_->finished(reply);
    });
    QObject::connect(manager, &QObject::destroyed, this, [manager] {
        QMutexLocker lock(&managers_mutex);
        managers.remove(manager);
    });
}
ResourceCache::~ResourceCache() {
    invalidate();
    QMutexLocker lock(&managers_mutex);
    if (d_->manager && managers.value(d_->manager) == this) managers.remove(d_->manager);
}
auto ResourceCache::get(QNetworkAccessManager* manager) -> ResourceCache* {
    if (! manager || manager->thread() != QThread::currentThread()) return nullptr;
    QMutexLocker lock(&managers_mutex);
    return managers.value(manager);
}
void ResourceCache::invalidate(const QUrl& url) {
    if (url.isEmpty()) ++d_->epoch;
    for (auto it = d_->entries.begin(); it != d_->entries.end();) {
        if (url.isEmpty() || it.value()->url == url) {
            it.value()->invalid = true;
            it                  = d_->entries.erase(it);
        } else
            ++it;
    }
    QList<QPointer<QNetworkReply>> cancelled;
    for (auto weak : d_->operations) {
        if (auto op = weak.toStrongRef(); op && (url.isEmpty() || op->request.url() == url)) {
            op->discarded = true;
            if (op->selected) op->selected->invalid = true;
            if (op->reply && ! op->done) cancelled.append(op->reply);
        }
    }
    for (auto reply : cancelled)
        if (reply) reply->abort();
    for (const auto& observer : d_->observers)
        if (observer.receiver)
            QMetaObject::invokeMethod(
                observer.receiver,
                [callback = observer.callback, url] {
                    callback(url);
                },
                Qt::QueuedConnection);
}
void ResourceCache::clear() {
    if (d_->disk && d_->manager && d_->manager->cache() == d_->disk) {
        d_->disk->clear();
        return;
    }
    invalidate();
    if (d_->manager && d_->manager->cache()) d_->manager->cache()->clear();
}
void ResourceCache::observe(QObject* receiver, std::function<void(const QUrl&)> callback) {
    if (! receiver) return;
    d_->observers.removeIf([](const auto& observer) {
        return ! observer.receiver;
    });
    d_->observers.append({ receiver, std::move(callback) });
}
void ResourceCache::configure(ResourceOptions options) {
    if (options == d_->options) return;
    clear();
    d_->options = std::move(options);
}
auto ResourceCache::acquire(QNetworkRequest request) -> ResourceRequest {
    auto op     = QSharedPointer<ResourceOperation>::create();
    op->owner   = this;
    op->request = std::move(request);
    op->epoch   = d_->epoch;
    if (d_->permitted(op->request)) op->prior = d_->entries.value(key(op->request));
    return ResourceRequest(std::move(op));
}
auto ResourceCache::install_disk_cache(QString directory, qint64 maximum_bytes)
    -> QNetworkDiskCache* {
    if (! d_->manager || d_->manager->cache() || directory.isEmpty() || maximum_bytes <= 0)
        return nullptr;
    auto* disk = new ResourceDiskCache(this, d_->manager);
    disk->setCacheDirectory(std::move(directory));
    disk->setMaximumCacheSize(maximum_bytes);
    d_->manager->setCache(disk);
    d_->disk = disk;
    QObject::connect(disk, &QObject::destroyed, this, [this] {
        invalidate();
    });
    return disk;
}
ResourceRequest::ResourceRequest(QSharedPointer<ResourceOperation> operation)
    : operation_(std::move(operation)) {}
auto ResourceRequest::snapshot() const -> ResourceSnapshot {
    if (! operation_ || operation_->discarded) return {};
    if (operation_->selected) return ResourceSnapshot(operation_->selected);
    auto result = ResourceSnapshot(operation_->prior);
    return result.fresh() ? result : ResourceSnapshot();
}
auto ResourceRequest::open() -> QNetworkReply* {
    if (! operation_ || ! operation_->owner) return nullptr;
    return operation_->owner->d_->open(operation_);
}
void ResourceRequest::discard() {
    if (! operation_) return;
    operation_->discarded = true;
    if (operation_->selected) operation_->selected->invalid = true;
    if (operation_->reply && ! operation_->done) operation_->reply->abort();
}
} // namespace ncrequest::client::qt_network
