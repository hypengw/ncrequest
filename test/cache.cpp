#include <rstd/test/gtest.hpp>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
#include "support.hpp"
using namespace test_http;
TEST(HttpCache, SharedStoreAndPolicyReplacementHaveOneOwner) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  cache  = h::MemoryCache::make(engine.resources()).unwrap();
        auto  script = Script::make(State {});
        Reply cacheable;
        cacheable.headers.add("Cache-Control"_str, "max-age=60"_str).unwrap();
        script->lock().unwrap()->replies.push(rstd::move(cacheable));
        auto shared = h::BackendBinding::Shared(backend(script));
        auto config = [&]() {
            SessionOptions options;
            options.http.cache           = Some(h::StoreBinding::Shared(cache.store()));
            options.http.policy.cache    = h::CacheMode::Default;
            options.http.policy.redirect = Some(h::RedirectPolicy {});
            return options;
        };
        auto first    = Session::from_backend(engine.handle(), shared, config()).unwrap();
        auto second   = Session::from_backend(engine.handle(), shared, config()).unwrap();
        auto received = (co_await response(*first, request())).unwrap();
        EXPECT_FALSE(received->from_cache());
        EXPECT_TRUE((co_await received->bytes()).is_ok());
        EXPECT_TRUE((co_await first->close()).is_ok());
        auto hit = (co_await response(*second, request())).unwrap();
        EXPECT_TRUE(hit->from_cache());
        EXPECT_TRUE((co_await hit->bytes()).is_ok());
        EXPECT_EQ(script->lock().unwrap()->requests.len(), usize(1));
        Reply redirect;
        redirect.status = u16(302);
        redirect.headers.add("Location"_str, "/next"_str).unwrap();
        script->lock().unwrap()->replies.push(rstd::move(redirect));
        auto override                       = request();
        override.options().execution.policy = Some(h::ExecutionPolicy {});
        auto direct = (co_await response(*second, rstd::move(override))).unwrap();
        EXPECT_EQ(direct->head().status.value(), u16(302));
        EXPECT_FALSE(direct->from_cache());
        EXPECT_TRUE((co_await direct->bytes()).is_ok());
        EXPECT_EQ(script->lock().unwrap()->requests.len(), usize(2));
        EXPECT_TRUE((co_await second->close()).is_ok());
        EXPECT_TRUE((co_await cache.close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(HttpCache, TruncatedResponseCannotPublishAnEntry) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  cache  = h::MemoryCache::make(engine.resources()).unwrap();
        auto  script = Script::make(State {});
        Reply reply;
        reply.truncated = true;
        reply.headers.add("Cache-Control"_str, "max-age=60"_str).unwrap();
        script->lock().unwrap()->replies.push(rstd::move(reply));
        SessionOptions config;
        config.http.cache        = Some(h::StoreBinding::Shared(cache.store()));
        config.http.policy.cache = h::CacheMode::Default;
        auto session             = Session::from_backend(engine.handle(),
                                                         h::BackendBinding::Shared(backend(script)),
                                                         rstd::move(config))
                                       .unwrap();
        auto value               = (co_await response(*session, request())).unwrap();
        EXPECT_TRUE((co_await value->bytes()).is_err());
        auto               input = request();
        h::ExecutionPolicy policy;
        policy.cache                     = h::CacheMode::OnlyIfCached;
        input.options().execution.policy = Some(rstd::move(policy));
        auto miss                        = co_await response(*session, rstd::move(input));
        EXPECT_TRUE(miss.is_err());
        if (miss.is_err()) EXPECT_TRUE(miss.unwrap_err().as_Transport().cause.is_CacheMiss());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await cache.close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
