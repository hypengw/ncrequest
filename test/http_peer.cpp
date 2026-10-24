#include <rstd/test/gtest.hpp>
#include <stdlib.h>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
#include "support.hpp"
using namespace test_http;
namespace
{
auto env_text(const char* name) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(::getenv(name)).to_str().unwrap();
}
auto port(const char* name = "LIFETCHTO_PEER_PORT") -> u16 { return u16(::atoi(::getenv(name))); }
auto peer_request(ref<str> path) -> Request {
    return Request::from_url(rstd::format("http://127.0.0.1:{}{}", port(), path).as_str()).unwrap();
}
struct UploadCounts {
    usize pulls {}, releases {}, finished {};
};
struct UnpulledSource {
    Arc<rstd::sync::Mutex<UploadCounts>> counts;
    auto                                 next() -> coro<lf::Result<Option<Bytes>>> {
        ++counts->lock().unwrap()->pulls;
        co_return Ok(Some(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        ++counts->lock().unwrap()->releases;
        co_await rstd::async::sleep(Duration::from_millis(u64(10)));
        ++counts->lock().unwrap()->finished;
        co_return Ok(empty {});
    }
};
struct PeerFactory {
    Arc<rstd::sync::Mutex<usize>> opens;
    auto                          open(lf::TransferContext) const -> coro<lf::Result<UploadBody>> {
        ++*opens->lock().unwrap();
        co_return Ok(UploadBody::bytes(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> { co_return Ok(empty {}); }
};
} // namespace
namespace rstd
{
template<>
struct Impl<lifetchto::UploadFactory, PeerFactory> : ImplBase<PeerFactory> {
    auto open(lifetchto::TransferContext c) const { return this->self().open(rstd::move(c)); }
    void request_stop(lifetchto::StopReason r) const { this->self().request_stop(r); }
    auto release(lifetchto::ReleaseContext c) const { return this->self().release(c); }
};
} // namespace rstd
TEST(http, LocalHttpStreamsHeadersAndTransferredBody) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto session = Session::make(runtime.handle()).unwrap();
        for (auto path :
             array<ref<str>, 4> { "/ok"_str, "/informational"_str, "/eof"_str, "/204"_str }) {
            auto value    = (co_await response(*session, peer_request(path))).unwrap();
            auto expected = path == "/eof"_str   ? "eof-body"_str
                            : path == "/204"_str ? ""_str
                                                 : "hello"_str;
            EXPECT_EQ((co_await value->text()).unwrap().as_str(), expected);
        }
        auto value = (co_await response(*session, peer_request("/chunked"_str))).unwrap();
        EXPECT_EQ(value->header().get_all("x-dup"_str).len(), usize());
        auto body = value->take_body().unwrap();
        value.reset();
        EXPECT_EQ((co_await body.text()).unwrap().as_str(), "abcde"_str);
        EXPECT_EQ(*body.trailers().unwrap()->get_unique_text("x-end"_str).unwrap(), "yes"_str);
        auto head = (co_await response(*session, peer_request("/ok"_str))).unwrap();
        EXPECT_EQ(head->header().get_all("x-dup"_str).len(), usize(2));
        EXPECT_EQ((*head->header().get("x-bin"_str))->as_slice()[usize()], u8(255));
        EXPECT_TRUE((co_await head->bytes()).is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, LocalHttpMethodBodyAndConvenience) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto session = Session::make(runtime.handle()).unwrap();
        auto input   = peer_request("/echo"_str);
        input.try_set_method("PROPFIND"_str).unwrap();
        input.add_header("X-Dup"_str, "a"_str.as_bytes()).unwrap();
        input.add_header("X-Dup"_str, "b"_str.as_bytes()).unwrap();
        array<u8, 1> raw { u8(255) };
        input.try_set_header("X-Bin"_str, raw.as_slice()).unwrap();
        input.text("custom"_str).unwrap();
        auto custom = (co_await response(*session, rstd::move(input))).unwrap();
        EXPECT_EQ((co_await custom->text()).unwrap().as_str(), "custom"_str);
        auto json = peer_request("/echo"_str);
        json.try_set_method("POST"_str).unwrap();
        auto object = rstd::json::from_str("{\"value\":1}"_str).unwrap();
        json.json(object).unwrap();
        auto encoded = (co_await response(*session, rstd::move(json))).unwrap();
        EXPECT_TRUE((co_await encoded->json()).is_ok());
        auto form = peer_request("/echo"_str);
        form.try_set_method("POST"_str).unwrap();
        lihttpto::QueryParams fields;
        fields.add("key"_str, "a b"_str);
        form.form(fields).unwrap();
        auto posted = (co_await response(*session, rstd::move(form))).unwrap();
        EXPECT_EQ((co_await posted->text()).unwrap().as_str(), "key=a+b"_str);
        auto post =
            session->post(peer_request("/echo"_str), Bytes::copy_from_slice("bytes"_str.as_bytes()))
                .unwrap();
        auto bytes = (co_await post.response()).unwrap();
        EXPECT_EQ((co_await bytes->text()).unwrap().as_str(), "bytes"_str);
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, LocalHttpRetryRedirectAndReplayHaveOnePolicyOwner) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        SessionOptions config;
        config.http.policy.redirect = Some(h::RedirectPolicy {});
        h::RetryPolicy retry;
        retry.methods.push("GET"_Str);
        retry.statuses.push(u16(503));
        retry.backoff            = Duration {};
        config.http.policy.retry = Some(rstd::move(retry));
        auto session             = Session::make(runtime.handle(), {}, rstd::move(config)).unwrap();
        auto retried = (co_await response(*session, peer_request("/policy-retry"_str))).unwrap();
        EXPECT_EQ((co_await retried->text()).unwrap().as_str(), "hello"_str);
        EXPECT_EQ((co_await retried->execution().wait()).unwrap().facts.attempts, usize(3));
        auto opens = Arc<rstd::sync::Mutex<usize>>::make(usize());
        auto input = peer_request("/policy-307"_str);
        input.try_set_method("POST"_str).unwrap();
        input.add_header("X-Dup"_str, "a"_str.as_bytes()).unwrap();
        input.add_header("X-Dup"_str, "b"_str.as_bytes()).unwrap();
        array<u8, 1> raw { u8(255) };
        input.try_set_header("X-Bin"_str, raw.as_slice()).unwrap();
        lihttpto::Headers trailers;
        trailers.add("X-End"_str, "yes"_str).unwrap();
        input.set_body(RequestBody::repeatable(
            { Some(u64(3)) },
            Arc<rstd::dyn<lf::UploadFactory>>::make(PeerFactory { opens.clone() }),
            Some(rstd::move(trailers))));
        auto replayed = (co_await response(*session, rstd::move(input))).unwrap();
        EXPECT_EQ((co_await replayed->text()).unwrap().as_str(), "abc"_str);
        EXPECT_EQ(*opens->lock().unwrap(), usize(2));
        EXPECT_EQ(replayed->request().url.request_target().as_str(), "/policy-upload"_str);
        auto rewrite = peer_request("/policy-303"_str);
        rewrite.try_set_method("POST"_str).unwrap();
        rewrite.set_body(Bytes::copy_from_slice("abc"_str.as_bytes()));
        auto rewritten = (co_await response(*session, rstd::move(rewrite))).unwrap();
        EXPECT_EQ(rewritten->request().method.as_ref(), "GET"_str);
        EXPECT_TRUE((co_await rewritten->bytes()).is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, LocalHttpCacheAndRevalidation) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto           engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto           backend = h::NativeBackend::make(engine.resources()).unwrap();
        auto           cache   = h::MemoryCache::make(engine.resources()).unwrap();
        SessionOptions config;
        config.http.policy.cache = h::CacheMode::Default;
        config.http.cache        = Some(h::StoreBinding::Shared(cache.store()));
        auto session = Session::from_http_backend(engine.handle(),
                                                  h::BackendBinding::Shared(backend.backend()),
                                                  rstd::move(config))
                           .unwrap();
        for (int i = 0; i < 2; ++i) {
            auto value = (co_await response(*session, peer_request("/cache"_str))).unwrap();
            EXPECT_EQ(value->from_cache(), i != 0);
            EXPECT_EQ((co_await value->text()).unwrap().as_str(), "hello"_str);
        }
        for (int i = 0; i < 2; ++i) {
            auto value = (co_await response(*session, peer_request("/revalidate"_str))).unwrap();
            EXPECT_EQ(value->from_cache(), i != 0);
            EXPECT_EQ((co_await value->text()).unwrap().as_str(), "hello"_str);
        }
        auto               input = peer_request("/cache-miss"_str);
        h::ExecutionPolicy policy;
        policy.cache                     = h::CacheMode::OnlyIfCached;
        input.options().execution.policy = Some(rstd::move(policy));
        auto miss                        = co_await response(*session, rstd::move(input));
        EXPECT_TRUE(miss.unwrap_err().as_Transport().cause.is_CacheMiss());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await cache.close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, LocalHttpDeadlineCancellationAndBodyDrop) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        SessionOptions config;
        config.http.total_timeout = Some(Duration::from_millis(u64(10)));
        auto session = Session::make(runtime.handle(), {}, rstd::move(config)).unwrap();
        auto input   = peer_request("/stall-head"_str);
        input.options().execution.total_timeout = Some(h::TotalTimeout::disabled());
        auto allowed = (co_await response(*session, rstd::move(input))).unwrap();
        EXPECT_TRUE((co_await allowed->bytes()).is_ok());
        auto slow = peer_request("/slow-body"_str);
        slow.options().execution.total_timeout =
            Some(h::TotalTimeout::after(Duration::from_millis(u64(30))));
        auto value = (co_await response(*session, rstd::move(slow))).unwrap();
        auto timed = co_await value->bytes();
        EXPECT_TRUE(timed.is_err());
        if (timed.is_err()) EXPECT_TRUE(timed.unwrap_err().as_Transport().cause.is_Timeout());
        auto control = Arc<RequestControl>::make();
        auto pending =
            session->send(peer_request("/stall-head"_str), Some(control.clone())).unwrap();
        control->cancel();
        auto canceled = co_await pending.response();
        EXPECT_TRUE(canceled.is_err());
        auto dropped    = (co_await response(*session, peer_request("/slow-body"_str))).unwrap();
        auto completion = dropped->completion();
        {
            auto body = dropped->take_body().unwrap();
        }
        EXPECT_TRUE((co_await completion.wait())->logical.result.is_err());
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, LocalHttpsAndUnixRouteUseLogicalIdentity) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto pem =
            rstd::fs::read(rstd::path::PathBuf::from(env_text("LIFETCHTO_CA")).as_path()).unwrap();
        lifetchto::http::TlsOptions trust;
        trust.use_system_roots = false;
        trust.ca_pem.push(Bytes::copy_from_slice(pem.as_slice()));
        h::TransportOptions transport;
        transport.tls = Some(h::TlsProfile::make(rstd::move(trust)).unwrap());
        auto session  = Session::make(runtime.handle(), rstd::move(transport)).unwrap();
        auto tls =
            Request::from_url(
                rstd::format("https://origin.test:{}/ok", port("LIFETCHTO_TLS_PORT")).as_str())
                .unwrap();
        Vec<rstd::net::SocketAddr> addresses;
        addresses.push(rstd::net::SocketAddr::ipv4_loopback(port("LIFETCHTO_TLS_PORT")));
        tls.options().execution.routing = Some(
            h::Routing { .route = Some(Arc<h::Route>::make(
                             h::Route::tcp(tls.url_info(), rstd::move(addresses)).unwrap())) });
        auto value = (co_await response(*session, rstd::move(tls))).unwrap();
        EXPECT_EQ((co_await value->text()).unwrap().as_str(), "hello"_str);
        auto unix_request = Request::from_url("http://unix.invalid/ok"_str).unwrap();
        unix_request.options().execution.routing = Some(
            h::Routing { .route = Some(Arc<h::Route>::make(
                             h::Route::unix_socket(
                                 unix_request.url_info(),
                                 Bytes::copy_from_slice(env_text("LIFETCHTO_UNIX_PATH").as_bytes()))
                                 .unwrap())) });
        auto unix_value = (co_await response(*session, rstd::move(unix_request))).unwrap();
        EXPECT_EQ((co_await unix_value->text()).unwrap().as_str(), "hello"_str);
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}

TEST(http, LocalHttpEarlyFinalResponseReleasesUnpulledUpload) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto session = Session::make(runtime.handle()).unwrap();
        auto counts  = Arc<rstd::sync::Mutex<UploadCounts>>::make(UploadCounts {});
        auto input   = peer_request("/expect-final"_str);
        input.try_set_method("POST"_str).unwrap();
        input.try_set_header("Expect"_str, "100-continue"_str).unwrap();
        input.set_body(RequestBody::once(
            UploadBody::from_source(UnpulledSource { counts.clone() }, Some(u64(3)))));
        auto value = (co_await response(*session, rstd::move(input))).unwrap();
        EXPECT_EQ(value->head().status.value(), u16(413));
        EXPECT_TRUE((co_await value->bytes(usize())).is_ok());
        EXPECT_EQ(counts->lock().unwrap()->pulls, usize());
        EXPECT_EQ(counts->lock().unwrap()->releases, usize(1));
        EXPECT_EQ(counts->lock().unwrap()->finished, usize(1));
        EXPECT_TRUE((co_await value->completion().wait())->logical.result.is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
