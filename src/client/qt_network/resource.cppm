module;
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSharedPointer>
#include <QList>
#include <memory>
#include <functional>
export module ncrequest:client.qt_network.resource;
export import :response;

export namespace ncrequest::client::qt_network
{
struct ResourceRecord;
struct ResourceOperation;
struct ResourceOptions {
    QList<QUrl>       origins;
    QList<QByteArray> excluded_response_headers;
    qint64            fallback_seconds {};
    qsizetype         maximum_entries { 256 };
    // Callers guarantee fixed request variants for every HTTP(S) origin.
    bool all_origins {};
    auto operator==(const ResourceOptions&) const -> bool = default;
};
struct ResourceStatistics {
    quint64   acquisitions {}, metadata_hits {}, retained_hits {}, fresh_hits {};
    quint64   index_evictions {}, opened {}, cache_results {};
    qsizetype indexed_entries {}, live_entries {};
};
class ResourceSnapshot {
    QSharedPointer<ResourceRecord> record_;
    friend class ResourceCache;
    friend class ResourceRequest;
    friend class ResourceDiskCache;
    explicit ResourceSnapshot(QSharedPointer<ResourceRecord>);

public:
    ResourceSnapshot() = default;
    auto identity() const -> QString;
    auto revision() const -> quint64;
    auto reusable() const -> bool;
    auto fresh() const -> bool;
};
class ResourceRequest {
    QSharedPointer<ResourceOperation> operation_;
    friend class ResourceCache;
    explicit ResourceRequest(QSharedPointer<ResourceOperation>);

public:
    ResourceRequest()                                          = default;
    ResourceRequest(ResourceRequest&&)                         = default;
    auto operator=(ResourceRequest&&) -> ResourceRequest&      = default;
    ResourceRequest(const ResourceRequest&)                    = delete;
    auto operator=(const ResourceRequest&) -> ResourceRequest& = delete;
    auto snapshot() const -> ResourceSnapshot;
    auto open() -> QNetworkReply*;
    void discard();
};
// Use on the manager's thread; allowed origins must use fixed request variants.
class ResourceCache : public QObject {
    class Private;
    std::unique_ptr<Private> d_;
    friend class ResourceRequest;
    friend class ResourceDiskCache;

public:
    explicit ResourceCache(QNetworkAccessManager*, ResourceOptions = {});
    ~ResourceCache() override;
    static auto get(QNetworkAccessManager*) -> ResourceCache*;
    auto        acquire(QNetworkRequest) -> ResourceRequest;
    auto        statistics() const -> ResourceStatistics;
    void        configure(ResourceOptions);
    void        invalidate(const QUrl& = {});
    void        clear();
    void        observe(QObject*, std::function<void(const QUrl&)>);
    auto        install_disk_cache(QString directory, qint64 maximum_bytes) -> QNetworkDiskCache*;
};
} // namespace ncrequest::client::qt_network
