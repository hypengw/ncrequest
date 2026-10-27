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
auto receive(qt::ResourceRequest& request) -> bool {
    auto* reply = request.open();
    if (! reply) return false;
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    if (! reply->isFinished()) loop.exec();
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
    EXPECT_TRUE(receive(fresh));
    auto identity = fresh.snapshot().identity();
    EXPECT_TRUE(fresh.snapshot().fresh());
    const auto requests = manager.requests;
    auto       hit      = acquire("/resource/fresh");
    EXPECT_TRUE(hit.snapshot().fresh() && hit.snapshot().identity() == identity);
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
    EXPECT_TRUE(validated.snapshot().fresh());

    auto changing = acquire("/resource/changing");
    EXPECT_TRUE(receive(changing));
    auto old     = changing.snapshot().identity();
    auto changed = acquire("/resource/changing");
    EXPECT_TRUE(receive(changed) && changed.snapshot().identity() != old);
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
#endif
