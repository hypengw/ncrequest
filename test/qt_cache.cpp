#include <rstd/test/gtest.hpp>
#include <cstdlib>
#if LITO_FEAT_QT
#    include <QCoreApplication>
#    include <QEventLoop>
#    include <QNetworkAccessManager>
#    include <QNetworkDiskCache>
#    include <QNetworkProxy>
#    include <QNetworkReply>
#    include <QPointer>
#    include <QThread>
#    include <QTimer>
#    include <string>
#    include <semaphore>
#    include <cstdio>
#    include <vector>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace h  = lifetchto::http;
namespace
{
struct Checks {
    std::vector<std::pair<std::string, bool>> results;
    void                                      add(const char* name, bool value) {
        results.emplace_back(name, value);
        std::fprintf(stderr, "cache check %s: %s\n", name, value ? "ok" : "FAILED");
    }
    void verify() const {
        for (const auto& [name, value] : results) {
            SCOPED_TRACE(name.c_str());
            EXPECT_TRUE(value);
        }
    }
};
struct Stats {
    int prepared {}, inserted {}, removed {};
};
class DiskCache : public QNetworkDiskCache {
public:
    Stats             stats;
    bool              fail {};
    Option<lf::Event> on_insert;
    QIODevice*        prepare(const QNetworkCacheMetaData& data) override {
        ++stats.prepared;
        return fail ? nullptr : QNetworkDiskCache::prepare(data);
    }
    void insert(QIODevice* device) override {
        ++stats.inserted;
        QNetworkDiskCache::insert(device);
        if (on_insert.is_some()) on_insert->notify();
    }
    bool remove(const QUrl& url) override {
        ++stats.removed;
        return QNetworkDiskCache::remove(url);
    }
};
struct QtOwner {
    QThread                thread;
    QObject                bridge;
    QNetworkAccessManager* manager {};
    DiskCache*             cache {};
    QtOwner() {
        thread.start();
        bridge.moveToThread(&thread);
    }
    template<class F>
    void call(F&& f) {
        QMetaObject::invokeMethod(&bridge, std::forward<F>(f), Qt::BlockingQueuedConnection);
    }
    void create(const QString& path) {
        call([&] {
            manager = new QNetworkAccessManager;
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            cache = new DiskCache;
            cache->setCacheDirectory(path);
            manager->setCache(cache);
        });
    }
    bool seed(const QString& address) {
        bool okay {};
        call([&] {
            QEventLoop      loop;
            QNetworkRequest request { QUrl(address) };
            request.setRawHeader("Accept-Encoding", "identity");
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            auto* reply = manager->get(request);
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            loop.exec();
            okay = reply->isFinished() && reply->error() == QNetworkReply::NoError &&
                   reply->readAll() == "cache-body";
            delete reply;
        });
        return okay;
    }
    ~QtOwner() {
        call([&] {
            delete manager;
            bridge.moveToThread(QCoreApplication::instance()->thread());
        });
        thread.quit();
        thread.wait();
    }
};
auto input(ref<str> path) -> Request {
    auto url = rstd::ffi::CStr::from_ptr(std::getenv("NCREQUEST_QT_CACHE_URL")).to_str().unwrap();
    return Request::from_url(rstd::format("{}{}", url, path).as_str()).unwrap();
}
auto config() -> SessionOptions {
    SessionOptions options;
    options.http.cache        = Some(h::CacheBinding::Backend());
    options.http.policy.cache = h::CacheMode::Default;
    return options;
}
struct Received {
    bool                okay {}, cached {};
    h::CacheObservation cache;
    lf::RemoteOutcome   remote { lf::RemoteOutcome::Unknown };
    usize               attempts {};
};
auto fetch(Arc<Session> session, Request request) -> coro<Received> {
    bool head      = request.method().as_ref() == "HEAD"_str;
    auto submitted = session->send(rstd::move(request));
    if (submitted.is_err()) co_return Received {};
    auto pending = rstd::move(submitted).unwrap();
    auto result  = co_await pending.response();
    if (result.is_err()) {
        (void)co_await pending.close();
        co_return Received {};
    }
    auto response = rstd::move(result).unwrap();
    auto bytes    = co_await response->bytes();
    auto report   = co_await response->completion().wait();
    co_return Received { bytes.is_ok() &&
                             bytes->as_slice() == (head ? ""_str : "cache-body"_str).as_bytes() &&
                             report->logical.result.is_ok(),
                         response->from_cache(),
                         response->cache(),
                         report->logical.remote,
                         response->execution().snapshot().attempts };
}
} // namespace
TEST(AdapterPeer, QtNativeCacheExternalManager) {
    if (! std::getenv("NCREQUEST_QT_CACHE_URL")) GTEST_SKIP();
    Checks  checks;
    auto    runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QtOwner owner;
    auto    base = QString::fromUtf8(std::getenv("NCREQUEST_QT_CACHE_URL"));
    owner.create(QString::fromUtf8(std::getenv("NCREQUEST_QT_CACHE_DIR")) + "/external");
    checks.add("direct QNAM prepopulation", owner.seed(base + "/external/seed"));
    Option<client::qt_network::Backend> backend;
    owner.call([&] {
        client::qt_network::Options options;
        options.manager = owner.manager;
        options.cache   = Some(client::qt_network::NativeCache::Existing());
        backend         = Some(
            client::qt_network::Backend::make(engine.resources(), rstd::move(options)).unwrap());
    });
    auto task = [&]() -> coro<empty> {
        auto options                 = config();
        options.http.policy.redirect = Some(h::RedirectPolicy {});
        h::RetryPolicy retry;
        retry.methods.push("GET"_Str);
        retry.statuses.push(u16(503));
        retry.backoff             = lf::Duration {};
        options.http.policy.retry = Some(rstd::move(retry));
        auto session =
            Session::from_backend(engine.handle(), backend->shared_binding(), options).unwrap();
        auto sibling =
            Session::from_backend(engine.handle(), backend->shared_binding(), options).unwrap();
        auto caps = session->cache_capabilities();
        checks.add("capability snapshot",
                   caps.configured && caps.readiness == h::CacheReadiness::Ready &&
                       caps.default_mode && ! caps.head && ! caps.revalidate &&
                       ! caps.only_if_cached && ! caps.store_errors && ! caps.network_observation &&
                       caps.isolation == h::CacheIsolation::Manager &&
                       caps.commit == h::CacheCommit::BackendCompletion);
        checks.add("same external cache",
                   (co_await fetch(session.clone(), input("/external/seed"_str))).cached);
        auto first = co_await fetch(session.clone(), input("/external/fresh"_str));
        auto hit   = co_await fetch(session.clone(), input("/external/fresh"_str));
        checks.add("first response from network",
                   first.okay && ! first.cached && first.remote == lf::RemoteOutcome::Confirmed);
        checks.add("cached origin is not no-network proof",
                   hit.okay && hit.cached && hit.cache.origin == h::CacheOrigin::BackendCache &&
                       hit.cache.network == h::NetworkAccess::Unknown &&
                       hit.remote == lf::RemoteOutcome::Unknown && hit.attempts == usize(1));
        auto stale     = co_await fetch(session.clone(), input("/external/stale"_str));
        auto validated = co_await fetch(session.clone(), input("/external/stale"_str));
        checks.add("Qt internal conditional validation",
                   stale.okay && ! stale.cached && validated.okay && validated.cached &&
                       validated.remote == lf::RemoteOutcome::Unknown &&
                       validated.cache.validation == h::CacheValidation::Unknown);
        for (auto path : array<ref<str>, 2> { "/external/chunked"_str, "/external/no-store"_str }) {
            auto a = co_await fetch(session.clone(), input(path));
            auto b = co_await fetch(session.clone(), input(path));
            checks.add("native chunked/no-store",
                       a.okay && b.okay && ! a.cached &&
                           b.cached == (path == "/external/chunked"_str));
        }
        for (auto mode : array<h::CacheMode, 2> { h::CacheMode::Disabled, h::CacheMode::Bypass }) {
            auto path =
                mode == h::CacheMode::Disabled ? "/external/disabled"_str : "/external/bypass"_str;
            auto request                              = input(path);
            request.options().execution.policy        = Some(h::ExecutionPolicy {});
            request.options().execution.policy->cache = mode;
            auto a = co_await fetch(session.clone(), rstd::move(request));
            auto b = co_await fetch(session.clone(), input(path));
            auto c = co_await fetch(session.clone(), input(path));
            checks.add("disabled does not write",
                       a.okay && ! a.cached && b.okay && ! b.cached && c.okay && c.cached);
            auto existing                              = input("/external/fresh"_str);
            existing.options().execution.policy        = Some(h::ExecutionPolicy {});
            existing.options().execution.policy->cache = mode;
            auto ignored = co_await fetch(session.clone(), rstd::move(existing));
            checks.add("disabled does not read", ignored.okay && ! ignored.cached);
        }
        lf::Event published;
        owner.call([&] {
            owner.cache->on_insert = Some(published.clone());
        });
        auto publication = published.listen();
        auto unconsumed  = session->send(input("/external/commit"_str)).unwrap();
        auto committed   = co_await rstd::async::select(
            rstd::move(publication), rstd::async::sleep(lf::Duration::from_secs(u64(2))));
        owner.call([&] {
            owner.cache->on_insert = None();
        });
        checks.add("native publication precedes application EOF", committed.is_left());
        unconsumed.cancel();
        (void)co_await unconsumed.close();
        checks.add("later application cancel does not roll back Qt cache",
                   (co_await fetch(session.clone(), input("/external/commit"_str))).cached);
        auto partial          = session->send(input("/external/cancel"_str)).unwrap();
        auto partial_response = (co_await partial.response()).unwrap();
        auto partial_body     = partial_response->take_body().unwrap();
        auto part             = co_await partial_body.next();
        checks.add("partial body received", part.is_ok() && part->is_some());
        partial_response->cancel();
        (void)co_await partial_body.close();
        auto after_cancel = co_await fetch(session.clone(), input("/external/cancel"_str));
        checks.add("partial native reply is not published",
                   after_cancel.okay && ! after_cancel.cached);
        auto           store = h::MemoryCache::make(engine.resources()).unwrap();
        SessionOptions common;
        common.http.cache = Some(h::CacheBinding::Store(h::StoreBinding::Shared(store.store())));
        common.http.policy.cache  = h::CacheMode::Default;
        common.http.store_failure = h::StoreFailure::Fail;
        auto store_session =
            Session::from_backend(engine.handle(), backend->shared_binding(), common).unwrap();
        Stats before, after;
        owner.call([&] {
            before = owner.cache->stats;
        });
        auto common_first = co_await fetch(store_session.clone(), input("/external/store"_str));
        auto common_hit   = co_await fetch(store_session.clone(), input("/external/store"_str));
        owner.call([&] {
            after = owner.cache->stats;
        });
        checks.add("Store and QNAM do not both write",
                   common_first.okay && common_hit.okay && common_hit.cached &&
                       common_hit.cache.origin == h::CacheOrigin::Store &&
                       common_hit.remote == lf::RemoteOutcome::NotApplicable &&
                       common_hit.attempts == usize() && before.prepared == after.prepared &&
                       before.inserted == after.inserted);
        (void)co_await store_session->close();
        (void)co_await store.close();
        auto cookie_first = co_await fetch(session.clone(), input("/external/set-cookie"_str));
        auto cookie_hit   = co_await fetch(session.clone(), input("/external/set-cookie"_str));
        checks.add("QNAM may cache Set-Cookie responses under native policy",
                   cookie_first.okay && cookie_hit.okay && cookie_hit.cached);
        auto variant_a = input("/external/vary"_str);
        variant_a.try_set_header("X-Variant"_str, "a"_str).unwrap();
        auto variant_b = input("/external/vary"_str);
        variant_b.try_set_header("X-Variant"_str, "b"_str).unwrap();
        auto varying_first = co_await fetch(session.clone(), rstd::move(variant_a));
        auto varying_hit   = co_await fetch(session.clone(), rstd::move(variant_b));
        checks.add("native Vary differs from Store and is not advertised",
                   varying_first.okay && varying_hit.okay && varying_hit.cached &&
                       ! session->cache_capabilities().vary);
        checks.add("bypass preserves unrelated cache",
                   (co_await fetch(session.clone(), input("/external/seed"_str))).cached);
        auto head = input("/external/head"_str);
        head.try_set_method("HEAD"_str).unwrap();
        checks.add("HEAD transmitted without native caching",
                   (co_await fetch(session.clone(), rstd::move(head))).okay);
        checks.add("HEAD does not populate GET",
                   ! (co_await fetch(session.clone(), input("/external/head"_str))).cached);
        checks.add("GET after HEAD caches complete body",
                   (co_await fetch(session.clone(), input("/external/head"_str))).cached);
        for (int i {}; i < 2; ++i)
            checks.add("truncated response rejected",
                       ! (co_await fetch(session.clone(), input("/external/truncated"_str))).okay);
        checks.add("redirect preserves native provider",
                   (co_await fetch(session.clone(), input("/external/redirect"_str))).cached);
        auto retried = co_await fetch(session.clone(), input("/external/retry"_str));
        checks.add("retry preserves native directive",
                   retried.okay && retried.attempts == usize(2));
        for (auto mode :
             array<h::CacheMode, 2> { h::CacheMode::Revalidate, h::CacheMode::OnlyIfCached }) {
            auto request                              = input("/rejected-mode"_str);
            request.options().execution.policy        = Some(h::ExecutionPolicy {});
            request.options().execution.policy->cache = mode;
            auto rejected                             = session->send(rstd::move(request));
            checks.add("mode rejected before dispatch",
                       rejected.is_err() && rejected.as_ref().unwrap_err().error.is_Unsupported());
        }
        auto partition                          = input("/rejected-partition"_str);
        partition.options().execution.partition = "account"_Str;
        checks.add("partition rejected", session->send(rstd::move(partition)).is_err());
        for (auto field : array<ref<str>, 4> {
                 "Authorization"_str, "Cookie"_str, "Range"_str, "If-None-Match"_str }) {
            auto request = input("/rejected-field"_str);
            request.try_set_header(field, "value"_str).unwrap();
            checks.add("identity/range/conditional request rejected",
                       session->send(rstd::move(request)).is_err());
        }
        auto strict               = config();
        strict.http.store_failure = h::StoreFailure::Fail;
        checks.add(
            "strict storage failure rejected",
            Session::from_backend(engine.handle(), backend->shared_binding(), strict).is_err());
        auto slow = input("/external/slow"_str);
        slow.options().execution.total_timeout =
            Some(h::TotalTimeout::after(lf::Duration::from_millis(u64(20))));
        checks.add("deadline cancels native reply",
                   ! (co_await fetch(session.clone(), rstd::move(slow))).okay);
        checks.add("shared session close", (co_await session->close()).is_ok());
        checks.add("sibling survives shared close",
                   (co_await fetch(sibling.clone(), input("/external/seed"_str))).cached);
        auto owned =
            Session::from_backend(engine.handle(), backend->owned_binding(), config()).unwrap();
        checks.add("owned backend close", (co_await owned->close()).is_ok());
        checks.add("sibling stops after owned backend close",
                   ! (co_await fetch(sibling.clone(), input("/external/seed"_str))).okay);
        checks.add("repeat close barrier", (co_await backend->wait_closed()).is_ok());
        checks.add("close sibling", (co_await sibling->close()).is_ok());
        checks.add("engine close", (co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
    checks.add("external QNAM and cache survive backend close",
               owner.seed(base + "/external/seed"));
    checks.verify();
}
TEST(AdapterPeer, QtNativeCacheInternalDiskAndFailures) {
    if (! std::getenv("NCREQUEST_QT_CACHE_URL")) GTEST_SKIP();
    Checks checks;
    auto   runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto   task    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        auto directory =
            rstd::ffi::CStr::from_ptr(std::getenv("NCREQUEST_QT_CACHE_DIR")).to_str().unwrap();
        for (int i {}; i < 2; ++i) {
            client::qt_network::Options options;
            options.cache =
                Some(client::qt_network::NativeCache::Disk(client::qt_network::DiskCacheOptions {
                    rstd::format("{}/internal", directory), i64(1024 * 1024) }));
            auto backend =
                client::qt_network::Backend::make(engine.resources(), rstd::move(options)).unwrap();
            auto session =
                Session::from_backend(engine.handle(), backend.owned_binding(), config()).unwrap();
            auto received = co_await fetch(session.clone(), input("/internal/persist"_str));
            checks.add("disk persists after real backend teardown",
                       received.okay && received.cached == (i != 0));
            checks.add("internal cache initialized on owner",
                       backend.capabilities().cache.readiness == h::CacheReadiness::Ready);
            checks.add("internal session close", (co_await session->close()).is_ok());
            checks.add("internal repeated close", (co_await backend.wait_closed()).is_ok());
        }
        for (auto suffix : array<ref<str>, 2> { "/capacity"_str, "/invalid"_str }) {
            client::qt_network::Options options;
            options.cache =
                Some(client::qt_network::NativeCache::Disk(client::qt_network::DiskCacheOptions {
                    suffix == "/invalid"_str ? "/dev/null/cache"_Str
                                             : rstd::format("{}{}", directory, suffix),
                    i64(1) }));
            auto backend =
                client::qt_network::Backend::make(engine.resources(), rstd::move(options)).unwrap();
            auto session =
                Session::from_backend(engine.handle(), backend.owned_binding(), config()).unwrap();
            for (int i {}; i < 2; ++i) {
                auto received =
                    co_await fetch(session.clone(),
                                   input(suffix == "/invalid"_str ? "/rejected-directory"_str
                                                                  : "/capacity/cache"_str));
                checks.add("unusable directory rejects; small cache is best effort",
                           suffix == "/invalid"_str ? ! received.okay
                                                    : received.okay && ! received.cached);
            }
            if (suffix == "/invalid"_str)
                checks.add("initialization failure is observable",
                           backend.capabilities().cache.readiness == h::CacheReadiness::Failed);
            (void)co_await session->close();
        }
        (void)co_await engine.close();
        auto used = engine.resources().usage();
        checks.add("internal resource quota returned",
                   used.operations == usize() && used.tasks == usize() &&
                       used.resources == usize() && used.bytes == usize());
        co_return empty {};
    };
    runtime.block_on(task());
    checks.verify();
}
TEST(AdapterPeer, QtNativeCacheStorageFailureAndLifetime) {
    if (! std::getenv("NCREQUEST_QT_CACHE_URL")) GTEST_SKIP();
    Checks  checks;
    auto    runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QtOwner owner;
    owner.create(QString::fromUtf8(std::getenv("NCREQUEST_QT_CACHE_DIR")) + "/failure");
    Option<client::qt_network::Backend> backend;
    owner.call([&] {
        owner.cache->fail = true;
        client::qt_network::Options options;
        options.manager = owner.manager;
        options.cache   = Some(client::qt_network::NativeCache::Existing());
        backend         = Some(
            client::qt_network::Backend::make(engine.resources(), rstd::move(options)).unwrap());
    });
    auto task = [&]() -> coro<empty> {
        auto session =
            Session::from_backend(engine.handle(), backend->shared_binding(), config()).unwrap();
        for (int i {}; i < 2; ++i) {
            auto received = co_await fetch(session.clone(), input("/failure/cache"_str));
            checks.add("storage rejection leaves successful network response",
                       received.okay && ! received.cached);
        }
        Stats stats;
        owner.call([&] {
            stats = owner.cache->stats;
        });
        checks.add("QNAM attempted native storage", stats.prepared > 0 && stats.inserted == 0);
        auto pending = session->send(input("/failure/slow"_str)).unwrap();
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(5)));
        std::binary_semaphore entered(0), unblock(0);
        QMetaObject::invokeMethod(
            &owner.bridge,
            [&] {
                entered.release();
                unblock.acquire();
            },
            Qt::QueuedConnection);
        entered.acquire();
        auto close = co_await rstd::async::select(
            backend->close().into_future(), rstd::async::sleep(lf::Duration::from_millis(u64(2))));
        checks.add("close waits for Qt owner; cancelling waiter is safe", close.is_right());
        unblock.release();
        checks.add("cancelled close can be reawaited", (co_await backend->close()).is_ok());
        (void)co_await pending.close();
        checks.add("repeated real exit barrier", (co_await backend->wait_closed()).is_ok());
        checks.add("external session close", (co_await session->close()).is_ok());
        (void)co_await engine.close();
        auto used = engine.resources().usage();
        checks.add("external quota returned",
                   used.operations == usize() && used.tasks == usize() &&
                       used.resources == usize() && used.bytes == usize());
        co_return empty {};
    };
    runtime.block_on(task());
    checks.verify();
}
TEST(HttpCache, NativeQtConfigurationAndExternalDestruction) {
    Checks checks;
    auto   runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto   engine  = lf::Engine::make(runtime.handle()).unwrap();
    client::qt_network::Options missing;
    missing.cache = Some(client::qt_network::NativeCache::Existing());
    checks.add("Existing requires an external cache",
               client::qt_network::Backend::make(engine.resources(), rstd::move(missing)).is_err());
    QtOwner owner;
    owner.create(QString::fromUtf8(std::getenv("NCREQUEST_QT_CACHE_DIR")
                                       ? std::getenv("NCREQUEST_QT_CACHE_DIR")
                                       : "/tmp/ncrequest-qnam-native-cache") +
                 "/destroyed");
    Option<client::qt_network::Backend> backend;
    bool                                no_cache_rejected {};
    owner.call([&] {
        client::qt_network::Options options;
        options.manager = owner.manager;
        auto uncached =
            client::qt_network::Backend::make(engine.resources(), rstd::move(options)).unwrap();
        no_cache_rejected =
            Session::from_backend(engine.handle(), uncached.shared_binding(), config()).is_err();
        client::qt_network::Options configured;
        configured.manager = owner.manager;
        configured.cache   = Some(client::qt_network::NativeCache::Existing());
        backend            = Some(
            client::qt_network::Backend::make(engine.resources(), rstd::move(configured)).unwrap());
    });
    checks.add("external cache requires explicit opt-in", no_cache_rejected);
    owner.call([&] {
        delete owner.manager;
        owner.manager = nullptr;
        owner.cache   = nullptr;
    });
    auto task = [&]() -> coro<empty> {
        checks.add("external manager destroyed before close", (co_await backend->close()).is_ok());
        checks.add("external manager destruction repeat barrier",
                   (co_await backend->wait_closed()).is_ok());
        (void)co_await engine.close();
        auto used = engine.resources().usage();
        checks.add("destroyed owner returns all quota",
                   used.operations == usize() && used.tasks == usize() &&
                       used.resources == usize() && used.bytes == usize());
        co_return empty {};
    };
    runtime.block_on(task());
    checks.verify();
}

#endif
