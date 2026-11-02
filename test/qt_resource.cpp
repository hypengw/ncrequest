#include <rstd/test/gtest.hpp>
#include <cstdlib>
#if LITO_FEAT_QT
#    include <QEventLoop>
#    include <QNetworkAccessManager>
#    include <QNetworkDiskCache>
#    include <QNetworkReply>
#    include <QNetworkCookie>
#    include <QNetworkCookieJar>
#    include <QTemporaryDir>
#    include <QTimer>
import ncrequest;

namespace
{
namespace qt = ncrequest::client::qt_network;
class ResourceManager : public QNetworkAccessManager {
public:
    int            requests {};
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* body) override {
        ++requests;
        return QNetworkAccessManager::createRequest(operation, request, body);
    }
};
auto receive(qt::ResourceRequest& request, bool* cached = nullptr) -> bool {
    auto* reply = request.open();
    if (! reply) return false;
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    if (! reply->isFinished()) loop.exec();
    if (cached) *cached = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
    return reply->isFinished() && reply->error() == QNetworkReply::NoError &&
           reply->readAll() == "cache-body";
}
} // namespace

TEST(AdapterPeer, QtNativeCacheResourceMetadata) {
    auto* base = std::getenv("NCREQUEST_QT_CACHE_URL");
    if (! base) GTEST_SKIP();
    ResourceManager     manager;
    qt::ResourceOptions options;
    options.origins.append(QUrl(QString::fromUtf8(base)));
    options.fallback_seconds = 300;
    options.excluded_response_headers.append("X-Local-Resource");
    qt::ResourceCache resources(&manager, options);
    QTemporaryDir     directory;
    auto*             disk = resources.install_disk_cache(directory.path(), 1024 * 1024);
    EXPECT_TRUE(disk != nullptr);
    auto acquire = [&](const char* path) {
        return resources.acquire(
            QNetworkRequest(QUrl(QString::fromUtf8(base) + QString::fromUtf8(path))));
    };
    auto fresh = acquire("/resource/fresh");
    EXPECT_TRUE(! fresh.snapshot().fresh());
    EXPECT_TRUE(fresh.snapshot().revision() == 0);
    EXPECT_TRUE(receive(fresh));
    auto identity = fresh.snapshot().identity();
    EXPECT_TRUE(fresh.snapshot().fresh());
    EXPECT_TRUE(fresh.snapshot().revision() > 0);
    const auto requests = manager.requests;
    auto       hit      = acquire("/resource/fresh");
    EXPECT_TRUE(hit.snapshot().fresh() && hit.snapshot().identity() == identity);
    EXPECT_TRUE(hit.snapshot().revision() == fresh.snapshot().revision());
    EXPECT_TRUE(manager.requests == requests);
    disk->remove(QUrl(QString::fromUtf8(base) + "/resource/fresh"));
    EXPECT_TRUE(hit.snapshot().fresh());
    EXPECT_TRUE(acquire("/resource/fresh").snapshot().identity() == identity);

    auto fallback = acquire("/resource/fallback");
    EXPECT_TRUE(receive(fallback) && fallback.snapshot().fresh());
    EXPECT_TRUE(acquire("/resource/fallback").snapshot().fresh());
    auto stale = acquire("/resource/stale");
    EXPECT_TRUE(receive(stale) && ! stale.snapshot().fresh());
    auto staleIdentity = stale.snapshot().identity();
    auto validated     = acquire("/resource/stale");
    EXPECT_TRUE(receive(validated));
    EXPECT_TRUE(validated.snapshot().identity() == staleIdentity);
    EXPECT_TRUE(validated.snapshot().revision() > stale.snapshot().revision());
    EXPECT_TRUE(validated.snapshot().fresh());

    auto changing = acquire("/resource/changing");
    EXPECT_TRUE(receive(changing));
    auto old     = changing.snapshot().identity();
    auto changed = acquire("/resource/changing");
    EXPECT_TRUE(receive(changed) && changed.snapshot().identity() != old);
    EXPECT_TRUE(changed.snapshot().revision() > changing.snapshot().revision());
    for (auto path : { "/resource/no-store",
                       "/resource/no-cache",
                       "/resource/expired",
                       "/resource/max-age-zero",
                       "/resource/aged",
                       "/resource/local" }) {
        auto request = acquire(path);
        EXPECT_TRUE(receive(request));
        EXPECT_TRUE(! acquire(path).snapshot().fresh());
    }
    auto invalidated = hit.snapshot();
    disk->clear();
    EXPECT_TRUE(! invalidated.fresh());
    EXPECT_TRUE(! acquire("/resource/fresh").snapshot().fresh());
    auto reload = acquire("/resource/fresh");
    EXPECT_TRUE(receive(reload));
    EXPECT_TRUE(reload.snapshot().identity() != identity);
    auto disabled             = options;
    disabled.fallback_seconds = 0;
    resources.configure(disabled);
    auto zero = acquire("/resource/zero");
    EXPECT_TRUE(receive(zero) && ! zero.snapshot().fresh());

    auto complete = acquire("/resource/complete");
    EXPECT_TRUE(receive(complete) && complete.snapshot().fresh());
    auto  lease = complete.snapshot();
    auto* foreign =
        manager.get(QNetworkRequest(QUrl(QString::fromUtf8(base) + "/resource/complete")));
    QEventLoop foreignLoop;
    QObject::connect(foreign, &QNetworkReply::finished, &foreignLoop, &QEventLoop::quit);
    if (! foreign->isFinished()) foreignLoop.exec();
    foreign->deleteLater();
    EXPECT_TRUE(! lease.reusable());

    auto truncated = acquire("/resource/truncated");
    EXPECT_TRUE(! receive(truncated) && ! truncated.snapshot().reusable());
    auto  pending      = acquire("/resource/cancel");
    auto* pendingReply = pending.open();
    EXPECT_TRUE(pendingReply != nullptr);
    resources.clear();
    EXPECT_TRUE(pendingReply->isFinished());
    QEventLoop pendingLoop;
    QObject::connect(pendingReply, &QNetworkReply::finished, &pendingLoop, &QEventLoop::quit);
    if (! pendingReply->isFinished()) pendingLoop.exec();
    EXPECT_TRUE(! pending.snapshot().reusable());
    EXPECT_TRUE(! acquire("/resource/cancel").snapshot().reusable());

    auto anonymous = acquire("/resource/anonymous");
    EXPECT_TRUE(receive(anonymous) && anonymous.snapshot().fresh());
    manager.cookieJar()->setCookiesFromUrl({ QNetworkCookie("identity", "other") },
                                           QUrl(QString::fromUtf8(base)));
    EXPECT_TRUE(! anonymous.snapshot().reusable());
    EXPECT_TRUE(! acquire("/resource/anonymous").snapshot().fresh());
    auto authenticated = acquire("/resource/anonymous");
    EXPECT_TRUE(receive(authenticated) && ! authenticated.snapshot().reusable());
    manager.setCookieJar(new QNetworkCookieJar(&manager));

    auto replace = acquire("/resource/replace");
    EXPECT_TRUE(receive(replace) && replace.snapshot().fresh());
    auto replacedLease = replace.snapshot();
    manager.setCache(new QNetworkDiskCache(&manager));
    EXPECT_TRUE(! replacedLease.reusable());
    auto unsupported = acquire("/resource/replace");
    EXPECT_TRUE(receive(unsupported) && ! unsupported.snapshot().reusable());
}
TEST(AdapterPeer, QtNativeCacheResourceAllOrigins) {
    auto* base   = std::getenv("NCREQUEST_QT_CACHE_URL");
    auto* second = std::getenv("NCREQUEST_QT_RESOURCE_SECOND_URL");
    if (! base || ! second) GTEST_SKIP();
    QTemporaryDir directory;
    const QUrl    firstUrl(QString::fromUtf8(base) + "/resource/all-origins-first");
    const QUrl    secondUrl(QString::fromUtf8(second) + "/resource/all-origins-second");
    EXPECT_TRUE(firstUrl.port() != secondUrl.port());
    {
        ResourceManager   manager;
        qt::ResourceCache resources(&manager);
        auto* disk = resources.install_disk_cache(directory.path() + "/disabled", 1024 * 1024);
        ASSERT_TRUE(disk != nullptr);
        const QUrl url(QString::fromUtf8(base) + "/resource/default-disabled");
        for (int i = 0; i < 2; ++i) {
            auto request = resources.acquire(QNetworkRequest(url));
            bool cached {};
            EXPECT_TRUE(receive(request, &cached));
            EXPECT_TRUE(! cached && ! request.snapshot().reusable());
        }
        EXPECT_TRUE(! disk->metaData(url).isValid());
    }
    for (int generation = 0; generation < 2; ++generation) {
        ResourceManager     manager;
        qt::ResourceOptions options;
        options.all_origins = true;
        options.excluded_response_headers.append("X-Local-Resource");
        qt::ResourceCache resources(&manager, options);
        auto* disk = resources.install_disk_cache(directory.path() + "/all", 1024 * 1024);
        ASSERT_TRUE(disk != nullptr);
        for (const auto& url : { firstUrl, secondUrl }) {
            auto request = resources.acquire(QNetworkRequest(url));
            EXPECT_TRUE(! request.snapshot().fresh());
            bool cached {};
            EXPECT_TRUE(receive(request, &cached));
            EXPECT_TRUE(cached == (generation != 0));
            EXPECT_TRUE(request.snapshot().fresh());
            auto identity = request.snapshot().identity();
            auto hit      = resources.acquire(QNetworkRequest(url));
            EXPECT_TRUE(hit.snapshot().fresh() && hit.snapshot().identity() == identity);
            EXPECT_TRUE(receive(hit, &cached) && cached);
        }
        if (generation != 0) continue;
        for (const auto* path :
             { "/resource/all/no-store", "/resource/all/no-cache", "/resource/all/local" }) {
            for (int i = 0; i < 2; ++i) {
                auto request =
                    resources.acquire(QNetworkRequest(QUrl(QString::fromUtf8(base) + path)));
                bool cached {};
                EXPECT_TRUE(receive(request, &cached));
                EXPECT_TRUE(! cached && ! request.snapshot().fresh());
            }
        }
        for (const auto* header : { "Authorization", "Cookie" }) {
            const QUrl url(QString::fromUtf8(base) + "/resource/all/" +
                           QString::fromLatin1(header).toLower());
            for (int i = 0; i < 2; ++i) {
                QNetworkRequest input(url);
                input.setRawHeader(header, "test");
                auto request = resources.acquire(input);
                bool cached {};
                EXPECT_TRUE(receive(request, &cached));
                EXPECT_TRUE(! cached && ! request.snapshot().reusable());
            }
            EXPECT_TRUE(! disk->metaData(url).isValid());
        }
        auto data = resources.acquire(QNetworkRequest(QUrl("data:text/plain,cache-body")));
        EXPECT_TRUE(receive(data) && ! data.snapshot().reusable());
    }
}
TEST(AdapterPeer, QtNativeCacheResourceLru) {
    auto* base = std::getenv("NCREQUEST_QT_CACHE_URL");
    if (! base) GTEST_SKIP();
    ResourceManager     manager;
    qt::ResourceOptions options;
    options.all_origins     = true;
    options.maximum_entries = 2;
    qt::ResourceCache resources(&manager, options);
    QTemporaryDir     directory;
    ASSERT_TRUE(resources.install_disk_cache(directory.path(), 1024 * 1024));
    auto acquire = [&](const char* path) {
        return resources.acquire(QNetworkRequest(QUrl(QString::fromUtf8(base) + path)));
    };
    QString firstIdentity;
    {
        auto first = acquire("/resource/lru/first");
        ASSERT_TRUE(receive(first));
        firstIdentity = first.snapshot().identity();
    }
    {
        auto second = acquire("/resource/lru/second");
        ASSERT_TRUE(receive(second));
    }
    EXPECT_TRUE(acquire("/resource/lru/first").snapshot().identity() == firstIdentity);
    {
        auto third = acquire("/resource/lru/third");
        ASSERT_TRUE(receive(third));
    }
    EXPECT_TRUE(acquire("/resource/lru/first").snapshot().identity() == firstIdentity);
    EXPECT_TRUE(! acquire("/resource/lru/second").snapshot().fresh());
    EXPECT_TRUE(manager.requests == 3);
    auto stats = resources.statistics();
    EXPECT_TRUE(stats.indexed_entries == 2 && stats.live_entries == 2);
    EXPECT_TRUE(stats.index_evictions == 1 && stats.metadata_hits == 2 && stats.fresh_hits == 2);
    auto second = acquire("/resource/lru/second");
    bool cached {};
    EXPECT_TRUE(receive(second, &cached) && cached);
    stats = resources.statistics();
    EXPECT_TRUE(stats.opened == 4 && stats.cache_results == 1);
}
TEST(AdapterPeer, QtNativeCacheResourceRetainedMetadata) {
    auto* base = std::getenv("NCREQUEST_QT_CACHE_URL");
    if (! base) GTEST_SKIP();
    ResourceManager     manager;
    qt::ResourceOptions options;
    options.all_origins     = true;
    options.maximum_entries = 1;
    qt::ResourceCache resources(&manager, options);
    QTemporaryDir     directory;
    ASSERT_TRUE(resources.install_disk_cache(directory.path(), 1024 * 1024));
    auto url = [&](const char* path) {
        return QUrl(QString::fromUtf8(base) + path);
    };
    auto acquire = [&](const char* path) {
        return resources.acquire(QNetworkRequest(url(path)));
    };
    qt::ResourceSnapshot retained;
    {
        auto first = acquire("/resource/retained/first");
        ASSERT_TRUE(receive(first));
        retained = first.snapshot();
    }
    qt::ResourceSnapshot secondSnapshot;
    {
        auto second = acquire("/resource/retained/second");
        ASSERT_TRUE(receive(second));
        secondSnapshot = second.snapshot();
    }
    const auto requests = manager.requests;
    const auto identity = retained.identity();
    EXPECT_TRUE(retained.fresh());
    EXPECT_TRUE(acquire("/resource/retained/first").snapshot().identity() == identity);
    EXPECT_TRUE(manager.requests == requests);
    auto stats = resources.statistics();
    EXPECT_TRUE(stats.indexed_entries == 1 && stats.live_entries == 2);
    EXPECT_TRUE(stats.retained_hits == 1 && stats.fresh_hits == 1);
    resources.invalidate(url("/resource/retained/second"));
    EXPECT_TRUE(! secondSnapshot.reusable() && retained.fresh());
    resources.clear();
    EXPECT_TRUE(! retained.reusable());

    qt::ResourceSnapshot staleSnapshot;
    {
        auto stale = acquire("/resource/retained/stale");
        ASSERT_TRUE(receive(stale));
        staleSnapshot = stale.snapshot();
        EXPECT_TRUE(staleSnapshot.reusable() && ! staleSnapshot.fresh());
    }
    {
        auto other = acquire("/resource/retained/other");
        ASSERT_TRUE(receive(other));
    }
    auto validated = acquire("/resource/retained/stale");
    EXPECT_TRUE(receive(validated));
    EXPECT_TRUE(validated.snapshot().fresh());
    EXPECT_TRUE(validated.snapshot().identity() == staleSnapshot.identity());
    EXPECT_TRUE(! staleSnapshot.reusable());

    qt::ResourceSnapshot replaced;
    {
        auto changing = acquire("/resource/retained/changing");
        ASSERT_TRUE(receive(changing));
        replaced = changing.snapshot();
    }
    {
        auto other = acquire("/resource/retained/other");
        ASSERT_TRUE(receive(other));
    }
    auto changed = acquire("/resource/retained/changing");
    EXPECT_TRUE(receive(changed));
    EXPECT_TRUE(changed.snapshot().identity() != replaced.identity());
    EXPECT_TRUE(! replaced.reusable());
}
TEST(AdapterPeer, QtNativeCacheResourceZeroIndexAndDestruction) {
    auto* base = std::getenv("NCREQUEST_QT_CACHE_URL");
    if (! base) GTEST_SKIP();
    ResourceManager     manager;
    qt::ResourceOptions options;
    options.all_origins     = true;
    options.maximum_entries = 0;
    QTemporaryDir        directory;
    qt::ResourceSnapshot retained;
    const QUrl           url(QString::fromUtf8(base) + "/resource/retained/zero-index");
    {
        qt::ResourceCache resources(&manager, options);
        ASSERT_TRUE(resources.install_disk_cache(directory.path(), 1024 * 1024));
        {
            auto first = resources.acquire(QNetworkRequest(url));
            ASSERT_TRUE(receive(first));
            retained = first.snapshot();
        }
        EXPECT_TRUE(retained.fresh());
        EXPECT_TRUE(resources.statistics().indexed_entries == 0);
        EXPECT_TRUE(resources.acquire(QNetworkRequest(url)).snapshot().identity() ==
                    retained.identity());
        EXPECT_TRUE(resources.statistics().retained_hits == 1);
        EXPECT_TRUE(resources.statistics().opened == 1 && manager.requests == 1);
    }
    EXPECT_TRUE(! retained.reusable());
}
#endif
