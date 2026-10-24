#include <rstd/test/gtest.hpp>
#include <stdlib.h>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace lf = lifetchto;
namespace h  = lifetchto::http;
using lf::Arc;
using lf::Bytes;
using lf::coro;
auto variable(const char* name) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(::getenv(name)).to_str().unwrap();
}
template<class Stream>
auto read_exact(Stream&, usize) -> coro<String>;
TEST(ManagedDuplexPeer, CancellationDeadlineDropBoundsAndConnectFailure) {
    if (! ::getenv("LIFETCHTO_DUPLEX_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        for (int mode {}; mode < 2; ++mode) {
            if (mode == 1 && ! LITO_FEAT_CURL) continue;
            auto engine   = lf::Engine::make(runtime.handle()).unwrap();
            auto selected = [&]() -> Arc<rstd::dyn<h::ExchangeBackend>> {
#if LITO_FEAT_CURL
                if (mode == 1)
                    return ncrequest::client::curl::Backend::make(engine.resources())
                        .unwrap()
                        .backend();
#endif
                return h::NativeBackend::make(engine.resources()).unwrap().backend();
            }();
            auto session = ncrequest::Session::from_http_backend(
                               engine.handle(), h::BackendBinding::Owned(rstd::move(selected)))
                               .unwrap();
            for (int transport {}; transport < 2; ++transport) {
                auto target = [&]() {
                    h::DuplexRequest input { lihttpto::Url::parse_http(
                                                 rstd::format("http://127.0.0.1:{}/",
                                                              variable("LIFETCHTO_DUPLEX_PORT"))
                                                     .as_str())
                                                 .unwrap(),
                                             {} };
                    if (transport == 1)
                        input.route.route = Some(Arc<h::Route>::make(
                            h::Route::unix_socket(
                                input.url,
                                Bytes::copy_from_slice(
                                    variable("LIFETCHTO_DUPLEX_UNIX_PATH").as_bytes()))
                                .unwrap()));
                    return input;
                };
                for (int scenario {}; scenario < 3; ++scenario) {
                    h::DuplexOptions options;
                    options.total_timeout =
                        Some(h::TotalTimeout::after(lf::Duration::from_millis(u64(100))));
                    auto pending    = session->duplex(target(), options).unwrap();
                    auto completion = pending.completion();
                    if (scenario == 2) {
                        pending.cancel();
                        EXPECT_TRUE((co_await pending.connected()).is_err());
                    } else {
                        auto stream = (co_await pending.connected()).unwrap();
                        EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "ready"_str);
                        Vec<u8> large;
                        large.resize(usize(65537), u8());
                        EXPECT_TRUE(
                            (co_await stream.write(Bytes::copy_from_slice(large.as_slice())))
                                .unwrap_err()
                                .is_TooLarge());
                        if (scenario == 0) {
                            auto reading = stream.next().into_future();
                            auto race    = co_await rstd::async::select(
                                rstd::future::by_ref(reading),
                                rstd::async::sleep(lf::Duration::from_millis(u64(1))));
                            EXPECT_TRUE(race.is_right());
                        } else
                            EXPECT_TRUE((co_await stream.next()).unwrap_err().is_Timeout());
                    }
                    auto report = co_await completion.wait();
                    EXPECT_TRUE(report->logical.result.is_err());
                    EXPECT_TRUE(report->cleanup.is_ok());
                }
                auto dropped = Some(session->duplex(target()).unwrap());
                auto control = dropped->completion();
                dropped      = None();
                EXPECT_TRUE((co_await control.wait())->logical.result.is_err());
            }
            h::DuplexRequest missing {
                lihttpto::Url::parse_http("http://missing.invalid/"_str).unwrap(), {}
            };
            missing.route.route = Some(Arc<h::Route>::make(
                h::Route::unix_socket(
                    missing.url,
                    Bytes::copy_from_slice("/missing/ncrequest-transport.sock"_str.as_bytes()))
                    .unwrap()));
            auto failed         = session->duplex(rstd::move(missing)).unwrap();
            auto connected      = co_await failed.connected();
            EXPECT_TRUE(connected.is_err());
            if (connected.is_err())
                EXPECT_TRUE(connected.as_ref().unwrap_err().clone().is_Connect() ||
                            connected.as_ref().unwrap_err().is_Extension());
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
        }
        co_return empty {};
    };
    runtime.block_on(task());
}
template<class Stream>
auto read_exact(Stream& stream, usize size) -> coro<String> {
    Vec<u8> bytes;
    while (bytes.len() < size) {
        auto next = (co_await stream.next()).unwrap();
        EXPECT_TRUE(next.is_some());
        if (next.is_none()) break;
        bytes.extend_from_slice(next->as_slice());
    }
    co_return String::from_utf8(rstd::move(bytes)).unwrap();
}
TEST(ManagedDuplexPeer, NativeTcpUnixAndTlsOwnTheirNativeExit) {
    if (! ::getenv("LIFETCHTO_DUPLEX_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        auto pem =
            rstd::fs::read(rstd::path::PathBuf::from(variable("LIFETCHTO_CA")).as_path()).unwrap();
        lifetchto::http::TlsOptions trust;
        trust.use_system_roots = false;
        trust.ca_pem.push(Bytes::copy_from_slice(pem.as_slice()));
        h::TransportOptions transport;
        transport.tls =
            Some(h::TlsProfile::make(rstd::move(trust), h::VersionPolicy::Http1).unwrap());
        auto backend = h::NativeBackend::make(engine.resources(), rstd::move(transport)).unwrap();
        auto session = ncrequest::Session::from_http_backend(
                           engine.handle(), h::BackendBinding::Shared(backend.backend()))
                           .unwrap();
        for (int mode {}; mode < 3; ++mode) {
            auto           url = lihttpto::Url::parse_http(
                                     mode == 2 ? rstd::format("https://127.0.0.1:{}/",
                                                              variable("LIFETCHTO_DUPLEX_TLS_PORT"))
                                                     .as_str()
                                     : mode == 1 ? "http://unix.invalid/"_str
                                                 : rstd::format("http://127.0.0.1:{}/",
                                                                variable("LIFETCHTO_DUPLEX_PORT"))
                                                       .as_str())
                                     .unwrap();
            h::RouteChoice route;
            if (mode == 1)
                route.route = Some(Arc<h::Route>::make(
                    h::Route::unix_socket(
                        url,
                        Bytes::copy_from_slice(variable("LIFETCHTO_DUPLEX_UNIX_PATH").as_bytes()))
                        .unwrap()));
            auto pending = session->duplex({ rstd::move(url), rstd::move(route) }).unwrap();
            auto stream  = (co_await pending.connected()).unwrap();
            EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "ready"_str);
            EXPECT_EQ(
                (co_await stream.write(Bytes::copy_from_slice("hello"_str.as_bytes()))).unwrap(),
                usize(5));
            EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "hello"_str);
            EXPECT_EQ(stream.can_close_input(), mode != 2);
            if (mode != 2) {
                EXPECT_TRUE((co_await stream.close_input()).is_ok());
                EXPECT_EQ((co_await read_exact(stream, usize(9))).as_str(), "after-eof"_str);
                EXPECT_TRUE((co_await stream.next()).unwrap().is_none());
            } else {
                EXPECT_TRUE((co_await stream.close_input()).unwrap_err().is_Unsupported());
                EXPECT_EQ((co_await stream.write(Bytes::copy_from_slice("again"_str.as_bytes())))
                              .unwrap(),
                          usize(5));
                EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "again"_str);
            }
            EXPECT_TRUE(stream.finish().is_ok());
            auto report = co_await stream.completion().wait();
            EXPECT_TRUE(report->logical.result.is_ok());
            EXPECT_TRUE(report->cleanup.is_ok());
        }
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
#if LITO_FEAT_CURL
TEST(ManagedDuplexPeer, CurlUsesSelectedBackendForTcpUnixAndTls) {
    if (! ::getenv("LIFETCHTO_DUPLEX_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto                             engine = lf::Engine::make(runtime.handle()).unwrap();
        ncrequest::client::curl::Options options;
        auto                             pem =
            rstd::fs::read(rstd::path::PathBuf::from(variable("LIFETCHTO_CA")).as_path()).unwrap();
        options.tls.use_system_roots = false;
        options.tls.ca_pem.push(Bytes::copy_from_slice(pem.as_slice()));
        auto backend =
            ncrequest::client::curl::Backend::make(engine.resources(), rstd::move(options))
                .unwrap();
        auto session = ncrequest::Session::from_http_backend(
                           engine.handle(), h::BackendBinding::Shared(backend.backend()))
                           .unwrap();
        EXPECT_TRUE(session->capabilities().duplex);
        for (int mode {}; mode < 3; ++mode) {
            auto           url = lihttpto::Url::parse_http(
                                     mode == 2 ? rstd::format("https://127.0.0.1:{}/",
                                                              variable("LIFETCHTO_DUPLEX_TLS_PORT"))
                                                     .as_str()
                                     : mode == 1 ? "http://unix.invalid/"_str
                                                 : rstd::format("http://127.0.0.1:{}/",
                                                                variable("LIFETCHTO_DUPLEX_PORT"))
                                                       .as_str())
                                     .unwrap();
            h::RouteChoice route;
            if (mode == 1)
                route.route = Some(Arc<h::Route>::make(
                    h::Route::unix_socket(
                        url,
                        Bytes::copy_from_slice(variable("LIFETCHTO_DUPLEX_UNIX_PATH").as_bytes()))
                        .unwrap()));
            auto pending = session->duplex({ rstd::move(url), rstd::move(route) }).unwrap();
            auto stream  = (co_await pending.connected()).unwrap();
            EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "ready"_str);
            EXPECT_EQ(
                (co_await stream.write(Bytes::copy_from_slice("hello"_str.as_bytes()))).unwrap(),
                usize(5));
            EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "hello"_str);
            EXPECT_EQ(stream.can_close_input(), mode != 2);
            if (mode != 2) {
                EXPECT_TRUE((co_await stream.close_input()).is_ok());
                EXPECT_EQ((co_await read_exact(stream, usize(9))).as_str(), "after-eof"_str);
                EXPECT_TRUE((co_await stream.next()).unwrap().is_none());
            } else {
                EXPECT_TRUE((co_await stream.close_input()).unwrap_err().is_Unsupported());
                EXPECT_TRUE((co_await stream.write(Bytes::copy_from_slice("again"_str.as_bytes())))
                                .is_ok());
                EXPECT_EQ((co_await read_exact(stream, usize(5))).as_str(), "again"_str);
            }
            EXPECT_TRUE(stream.finish().is_ok());
            auto report = co_await stream.completion().wait();
            EXPECT_TRUE(report->logical.result.is_ok());
            EXPECT_TRUE(report->cleanup.is_ok());
        }
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
#endif
