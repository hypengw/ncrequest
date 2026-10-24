#include <rstd/test/gtest.hpp>
#include <stdlib.h>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace h  = lifetchto::http;
auto transport_env(const char* name) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(::getenv(name)).to_str().unwrap();
}
auto transport_file(const char* name) -> Bytes {
    auto data = rstd::fs::read(rstd::path::PathBuf::from(transport_env(name)).as_path()).unwrap();
    return Bytes::copy_from_slice(data.as_slice());
}
auto tls_backend(int mode, lf::ResourceContext resources, h::TlsOptions options,
                 h::ProxyOptions proxy = {}, h::TlsOptions proxy_tls = {})
    -> lf::Result<Arc<rstd::dyn<h::ExchangeBackend>>> {
#if LITO_FEAT_CURL
    if (mode == 1) {
        client::curl::Options config;
        config.tls       = rstd::move(options);
        config.proxy     = rstd::move(proxy);
        config.proxy_tls = rstd::move(proxy_tls);
        auto backend     = client::curl::Backend::make(rstd::move(resources), rstd::move(config));
        if (backend.is_err()) return Err(rstd::move(backend).unwrap_err());
        return Ok(backend->backend());
    }
#endif
#if LITO_FEAT_QT
    if (mode == 2) {
        client::qt_network::Options config;
        config.tls   = rstd::move(options);
        config.proxy = rstd::move(proxy);
        auto backend = client::qt_network::Backend::make(rstd::move(resources), rstd::move(config));
        if (backend.is_err()) return Err(rstd::move(backend).unwrap_err());
        return Ok(backend->backend());
    }
#endif
    h::TransportOptions config;
    config.proxy = rstd::move(proxy);
    auto profile = h::TlsProfile::make(rstd::move(options), h::VersionPolicy::Http1);
    if (profile.is_err()) return Err(rstd::move(profile).unwrap_err());
    config.tls   = Some(rstd::move(profile).unwrap());
    auto backend = h::NativeBackend::make(rstd::move(resources), rstd::move(config));
    if (backend.is_err()) return Err(rstd::move(backend).unwrap_err());
    return Ok(backend->backend());
}
TEST(TransportPeer, SharedTlsOptionsVerifyCaNameAndClientIdentity) {
    if (! ::getenv("LIFETCHTO_MTLS_PORT")) GTEST_SKIP();
    for (int mode {}; mode < 3; ++mode) {
#if ! LITO_FEAT_CURL
        if (mode == 1) continue;
#endif
#if ! LITO_FEAT_QT
        if (mode == 2) continue;
#endif
        auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
        auto test    = [&]() -> coro<empty> {
            auto engine = lf::Engine::make(runtime.handle()).unwrap();
            for (int scenario {}; scenario < 7; ++scenario) {
                h::TlsOptions options;
                options.use_system_roots = false;
                if (scenario < 2)
                    options.ca_pem.push(transport_file("LIFETCHTO_CA"));
                else if (scenario == 2 || scenario == 3)
                    options.ca_pem.push(transport_file("LIFETCHTO_WRONG_NAME_CA"));
                if (scenario == 5) options.use_system_roots = true;
                if (scenario == 6) {
                    options.ca_pem.push(transport_file("LIFETCHTO_CA"));
                    options.verification = h::TlsVerification::ChainOnly;
                }
                if (scenario == 0)
                    options.identity = Some(h::TlsIdentity {
                        transport_file("LIFETCHTO_CA"), transport_file("LIFETCHTO_CLIENT_KEY") });
                if (scenario == 3) options.verification = h::TlsVerification::ChainOnly;
                if (scenario == 4) options.verification = h::TlsVerification::Disabled;
                auto backend = tls_backend(mode, engine.resources(), rstd::move(options)).unwrap();
                auto session = Session::from_http_backend(
                                   engine.handle(), h::BackendBinding::Owned(rstd::move(backend)))
                                   .unwrap();
                auto target =
                    rstd::format("https://127.0.0.1:{}/ok",
                                 transport_env(scenario < 2    ? "LIFETCHTO_MTLS_PORT"
                                               : scenario == 5 ? "LIFETCHTO_TLS_PORT"
                                                               : "LIFETCHTO_WRONG_NAME_PORT"));
                auto pending  = session->get(Request::from_url(target.as_str()).unwrap()).unwrap();
                auto response = co_await pending.response();
                if (scenario == 1 || scenario == 2 || scenario == 5 || scenario == 6) {
                    EXPECT_TRUE(response.is_err());
                    if (response.is_err())
                        EXPECT_TRUE(response.as_ref().unwrap_err().is_Transport());
                } else {
                    EXPECT_TRUE(response.is_ok());
                    if (response.is_ok())
                        EXPECT_EQ((co_await response.unwrap()->text()).unwrap().as_str(),
                                  "hello"_str);
                }
                EXPECT_TRUE((co_await session->close()).is_ok());
            }
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
            co_return empty {};
        };
        runtime.block_on(test());
    }
}
TEST(TransportOptions, UnsupportedProfilesRejectBeforeStartingWorkers) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto                engine = lf::Engine::make(runtime.handle()).unwrap();
        h::TransportOptions native;
        native.low_speed = Some(h::LowSpeedOptions { u64(10), lf::Duration::from_secs(u64(1)) });
        EXPECT_TRUE(h::NativeBackend::make(engine.resources(), rstd::move(native))
                        .unwrap_err()
                        .is_Unsupported());
#if LITO_FEAT_QT
        client::qt_network::Options qt;
        qt.tcp.nodelay = Some(false);
        EXPECT_TRUE(client::qt_network::Backend::make(engine.resources(), rstd::move(qt))
                        .unwrap_err()
                        .is_Unsupported());
        client::qt_network::Options deadline;
        deadline.connect_timeout = Some(lf::Duration::from_secs(u64(1)));
        EXPECT_TRUE(client::qt_network::Backend::make(engine.resources(), rstd::move(deadline))
                        .unwrap_err()
                        .is_Unsupported());
        client::qt_network::Options slow;
        slow.low_speed = Some(h::LowSpeedOptions { u64(10), lf::Duration::from_secs(u64(1)) });
        EXPECT_TRUE(client::qt_network::Backend::make(engine.resources(), rstd::move(slow))
                        .unwrap_err()
                        .is_Unsupported());
#endif
#if LITO_FEAT_CURL
        client::curl::Options curl;
        curl.low_speed = Some(h::LowSpeedOptions { u64(10), lf::Duration::from_millis(u64(1)) });
        EXPECT_TRUE(client::curl::Backend::make(engine.resources(), rstd::move(curl))
                        .unwrap_err()
                        .is_InvalidOptions());
#endif
        EXPECT_EQ(engine.resources().usage().tasks, usize());
        EXPECT_EQ(engine.resources().usage().resources, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(TransportPeer, NativeTcpProfileIsAppliedBeforeHttp) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        h::TransportOptions options;
        options.tcp.nodelay = Some(false);
        rstd::net::TcpKeepalive keepalive;
        if (rstd::net::TcpStream::keepalive_timing_supported()) {
            keepalive.idle     = Some(lf::Duration::from_secs(u64(20)));
            keepalive.interval = Some(lf::Duration::from_secs(u64(5)));
        }
        options.tcp.keepalive = Some(keepalive);
        auto session          = Session::make(runtime.handle(), rstd::move(options)).unwrap();
        EXPECT_TRUE(session->capabilities().tcp.keepalive);
        EXPECT_TRUE(session->capabilities().connect_timeout);
        EXPECT_TRUE(! session->capabilities().unix_socket);
        auto request = Request::from_url(rstd::format("http://127.0.0.1:{}/ok",
                                                      transport_env("LIFETCHTO_PEER_PORT"))
                                             .as_str())
                           .unwrap();
        auto pending = session->send(rstd::move(request)).unwrap();
        EXPECT_EQ((co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                  "hello"_str);
        EXPECT_TRUE((co_await session->close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
#if LITO_FEAT_CURL
struct SlowUpload {
    bool sent {};
    auto next() -> coro<lf::Result<Option<Bytes>>> {
        if (rstd::exchange(sent, true)) co_return Ok(None());
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(1400)));
        co_return Ok(Some(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> { co_return Ok(empty {}); }
};
TEST(TransportPeer, CurlRatePolicyExcludesPausedUploadAndDownload) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto                  engine = lf::Engine::make(runtime.handle()).unwrap();
        client::curl::Options options;
        options.tcp.nodelay   = Some(false);
        options.tcp.keepalive = Some(rstd::net::TcpKeepalive {
            true, Some(lf::Duration::from_secs(u64(20))), Some(lf::Duration::from_secs(u64(5))) });
        options.low_speed =
            Some(h::LowSpeedOptions { u64(1000000), lf::Duration::from_secs(u64(1)) });
        auto backend =
            client::curl::Backend::make(engine.resources(), rstd::move(options)).unwrap();
        auto session = Session::from_http_backend(engine.handle(),
                                                  h::BackendBinding::Shared(backend.backend()))
                           .unwrap();
        EXPECT_TRUE(session->capabilities().low_speed);
        auto target = [&](ref<str> path) {
            return Request::from_url(rstd::format("http://127.0.0.1:{}{}",
                                                  transport_env("LIFETCHTO_PEER_PORT"),
                                                  path)
                                         .as_str())
                .unwrap();
        };
        auto pending  = session->get(target("/rate-slow-body"_str)).unwrap();
        auto response = (co_await pending.response()).unwrap();
        auto result   = co_await response->bytes();
        EXPECT_TRUE(result.is_err());
        if (result.is_err()) {
            auto error = rstd::move(result).unwrap_err();
            EXPECT_TRUE(error.is_Transport());
            auto& native = error.as_Transport().cause;
            EXPECT_TRUE(native.is_Extension());
            auto cause =
                rstd::error::downcast_ref<client::curl::Error>(native.as_Extension().cause.deref());
            EXPECT_TRUE(cause.is_some());
            EXPECT_EQ(cause->get().code(), i32(28));
        }
        auto large    = session->get(target("/large"_str)).unwrap();
        auto buffered = (co_await large.response()).unwrap();
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(1400)));
        EXPECT_EQ((co_await buffered->bytes()).unwrap().len(), usize(512 * 1024));
        auto upload = target("/echo"_str);
        upload.try_set_method("POST"_str).unwrap();
        upload.set_body(RequestBody::once(UploadBody::from_source(SlowUpload {}, Some(u64(3)))));
        auto sent = session->send(rstd::move(upload)).unwrap();
        EXPECT_EQ((co_await (co_await sent.response()).unwrap()->text()).unwrap().as_str(),
                  "abc"_str);
        auto raw = session->duplex({ target("/"_str).url_info().clone(), {} }).unwrap_err();
        EXPECT_TRUE(raw.error.is_Unsupported());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
#endif
auto proxy_profile(const char* port, ref<str> scheme = "http"_str) -> h::ProxyOptions {
    return h::ProxyOptions::explicit_proxy(
               lihttpto::Url::parse(
                   rstd::format("{}://127.0.0.1:{}", scheme, transport_env(port)).as_str())
                   .unwrap())
        .unwrap();
}
auto peer_trust() -> h::TlsOptions {
    h::TlsOptions options;
    options.use_system_roots = false;
    options.ca_pem.push(transport_file("LIFETCHTO_CA"));
    return options;
}
TEST(ProxyOptions, SnapshotSelectsCanonicalHostsPortsAndBypass) {
    h::ProxyEnvironment environment;
    environment.http     = Some("http://proxy.test:8080"_Str);
    environment.https    = Some("http://secure.test:8081"_Str);
    environment.all      = Some("socks5h://fallback.test:1081"_Str);
    environment.no_proxy = Some(".example.test, localhost:8088, [::1]:443, 127.0.0.1"_Str);
    auto profile         = h::ProxyOptions::system(rstd::move(environment)).unwrap();
    for (auto target : array<ref<str>, 5> { "http://example.test/"_str,
                                            "https://a.example.test/"_str,
                                            "http://localhost:8088/"_str,
                                            "https://[::1]/"_str,
                                            "http://127.0.0.1:99/"_str })
        EXPECT_TRUE(profile.select(lihttpto::Url::parse_http(target).unwrap()).unwrap().is_none());
    EXPECT_EQ(profile.select(lihttpto::Url::parse_http("http://other-example.test/"_str).unwrap())
                  .unwrap()
                  ->port(),
              u16(8080));
    EXPECT_EQ(profile.clone()
                  .select(lihttpto::Url::parse_http("https://localhost/"_str).unwrap())
                  .unwrap()
                  ->port(),
              u16(8081));
    auto ipv6 =
        h::ProxyEndpoint::make(lihttpto::Url::parse("http://[::1]:8080"_str).unwrap()).unwrap();
    EXPECT_EQ(ipv6.host_name().as_str(), "::1"_str);
    h::ProxyEnvironment invalid;
    invalid.no_proxy = Some("10.0.0.0/8"_Str);
    EXPECT_TRUE(h::ProxyOptions::system(rstd::move(invalid)).unwrap_err().is_InvalidOptions());
    EXPECT_TRUE(h::ProxyOptions::explicit_proxy(
                    lihttpto::Url::parse("http://user:secret@proxy.test"_str).unwrap())
                    .is_err());
    EXPECT_TRUE(
        h::ProxyOptions::explicit_proxy(lihttpto::Url::parse("http://proxy.test/path"_str).unwrap())
            .is_err());
}
TEST(ProxyPeer, HttpProxyConnectAndManagedDuplexUseSelectedBackend) {
    if (! ::getenv("LIFETCHTO_PROXY_PORT")) GTEST_SKIP();
    for (int mode {}; mode < 3; ++mode) {
#if ! LITO_FEAT_CURL
        if (mode == 1) continue;
#endif
#if ! LITO_FEAT_QT
        if (mode == 2) continue;
#endif
        auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
        auto test    = [&]() -> coro<empty> {
            auto engine = lf::Engine::make(runtime.handle()).unwrap();
            auto backend =
                tls_backend(
                    mode, engine.resources(), peer_trust(), proxy_profile("LIFETCHTO_PROXY_PORT"))
                    .unwrap();
            auto session = Session::from_http_backend(engine.handle(),
                                                      h::BackendBinding::Owned(rstd::move(backend)))
                               .unwrap();
            for (int repeat {}; repeat < 2; ++repeat) {
                auto url     = rstd::format("http://origin.test:{}/proxy-http?q=x%2Fy#ignored",
                                            transport_env("LIFETCHTO_PEER_PORT"));
                auto pending = session->get(Request::from_url(url.as_str()).unwrap()).unwrap();
                EXPECT_EQ(
                    (co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                    "hello"_str);
            }
            auto https =
                session
                    ->get(Request::from_url(rstd::format("https://origin.test:{}/proxy-https",
                                                         transport_env("LIFETCHTO_TLS_PORT"))
                                                .as_str())
                              .unwrap())
                    .unwrap();
            EXPECT_EQ((co_await (co_await https.response()).unwrap()->text()).unwrap().as_str(),
                      "hello"_str);
            auto denied =
                session
                    ->get(Request::from_url(rstd::format("https://deny.test:{}/proxy-denied",
                                                         transport_env("LIFETCHTO_TLS_PORT"))
                                                .as_str())
                              .unwrap())
                    .unwrap();
            auto rejected = co_await denied.response();
            EXPECT_TRUE(rejected.is_err());
            if (mode == 0 && rejected.is_err()) {
                auto error = rstd::move(rejected).unwrap_err();
                EXPECT_TRUE(error.as_Transport().cause.is_Protocol());
                auto cause = rstd::error::downcast_ref<h::ProxyConnectFailure>(
                    error.as_Transport().cause.as_Protocol().cause->deref());
                EXPECT_TRUE(cause.is_some());
                EXPECT_EQ(cause->get().response.status.value(), u16(407));
                EXPECT_TRUE(cause->get().body.as_slice() == "denied"_str.as_bytes());
                EXPECT_TRUE(cause->get().body_complete);
            }
            auto conflict = Request::from_url("http://origin.test/proxy-conflict"_str).unwrap();
            conflict.options().execution.routing = Some(h::Routing {
                .route = Some(Arc<h::Route>::make(
                    h::Route::unix_socket(conflict.url_info(),
                                          Bytes::copy_from_slice("/must-not-open"_str.as_bytes()))
                        .unwrap())) });
            auto refusal                         = session->send(rstd::move(conflict)).unwrap_err();
            EXPECT_TRUE(refusal.error.is_Unsupported());
            EXPECT_EQ(refusal.input.url(), "http://origin.test/proxy-conflict"_str);
            EXPECT_TRUE((co_await refusal.input.close()).is_ok());
            if (session->capabilities().duplex) {
                auto url =
                    lihttpto::Url::parse_http(rstd::format("http://origin.test:{}/",
                                                           transport_env("LIFETCHTO_DUPLEX_PORT"))
                                                  .as_str())
                        .unwrap();
                auto connecting = session->duplex({ rstd::move(url), {} }).unwrap();
                auto wire       = (co_await connecting.connected()).unwrap();
                EXPECT_TRUE((co_await wire.next()).unwrap()->as_slice() == "ready"_str.as_bytes());
                EXPECT_TRUE(
                    (co_await wire.write(Bytes::copy_from_slice("hello"_str.as_bytes()))).is_ok());
                EXPECT_TRUE((co_await wire.next()).unwrap()->as_slice() == "hello"_str.as_bytes());
                EXPECT_TRUE((co_await wire.close_input()).is_ok());
                EXPECT_TRUE((co_await wire.next()).unwrap()->as_slice() ==
                            "after-eof"_str.as_bytes());
                EXPECT_TRUE((co_await wire.next()).unwrap().is_none());
                EXPECT_TRUE(wire.finish().is_ok());
                EXPECT_TRUE((co_await wire.completion().wait())->cleanup.is_ok());
            }
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
            co_return empty {};
        };
        runtime.block_on(test());
    }
}
TEST(ProxyPeer, SystemSnapshotBypassAndBackendCapabilityRejections) {
    if (! ::getenv("LIFETCHTO_PROXY_PORT")) GTEST_SKIP();
    for (int mode {}; mode < 3; ++mode) {
#if ! LITO_FEAT_CURL
        if (mode == 1) continue;
#endif
#if ! LITO_FEAT_QT
        if (mode == 2) continue;
#endif
        auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
        auto test    = [&]() -> coro<empty> {
            auto                engine = lf::Engine::make(runtime.handle()).unwrap();
            h::ProxyEnvironment environment;
            environment.all =
                Some(rstd::format("http://127.0.0.1:{}", transport_env("LIFETCHTO_PROXY_PORT")));
            environment.no_proxy = Some("127.0.0.1"_Str);
            auto proxy           = h::ProxyOptions::system(rstd::move(environment)).unwrap();
            auto backend = tls_backend(mode, engine.resources(), peer_trust(), rstd::move(proxy));
            if (mode == 2)
                EXPECT_TRUE(backend.unwrap_err().is_Unsupported());
            else {
                auto session =
                    Session::from_http_backend(
                        engine.handle(), h::BackendBinding::Owned(rstd::move(backend).unwrap()))
                        .unwrap();
                for (auto host : array<ref<str>, 2> { "127.0.0.1"_str, "origin.test"_str }) {
                    auto pending = session
                                       ->get(Request::from_url(
                                                 rstd::format("http://{}:{}/proxy-snapshot",
                                                              host,
                                                              transport_env("LIFETCHTO_PEER_PORT"))
                                                     .as_str())
                                                 .unwrap())
                                       .unwrap();
                    EXPECT_EQ(
                        (co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                        "hello"_str);
                }
                EXPECT_TRUE((co_await session->close()).is_ok());
            }
            if (mode != 1) {
                auto unsupported =
                    tls_backend(mode,
                                engine.resources(),
                                peer_trust(),
                                proxy_profile("LIFETCHTO_HTTPS_PROXY_PORT", "https"_str));
                EXPECT_TRUE(unsupported.unwrap_err().is_Unsupported());
            }
            EXPECT_TRUE((co_await engine.close()).is_ok());
            co_return empty {};
        };
        runtime.block_on(test());
    }
}
#if LITO_FEAT_CURL
TEST(ProxyPeer, CurlHttpsAndSocksProxyProfiles) {
    if (! ::getenv("LIFETCHTO_PROXY_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        for (auto scheme : array<ref<str>, 5> {
                 "https"_str, "socks4"_str, "socks4a"_str, "socks5"_str, "socks5h"_str }) {
            auto proxy = proxy_profile(scheme == "https"_str ? "LIFETCHTO_HTTPS_PROXY_PORT"
                                                             : "LIFETCHTO_SOCKS_PORT",
                                       scheme);
            auto backend =
                tls_backend(1, engine.resources(), peer_trust(), rstd::move(proxy), peer_trust())
                    .unwrap();
            auto session = Session::from_http_backend(engine.handle(),
                                                      h::BackendBinding::Owned(rstd::move(backend)))
                               .unwrap();
            auto host    = scheme == "socks4"_str || scheme == "socks5"_str ? "127.0.0.1"_str
                                                                            : "origin.test"_str;
            auto target  = rstd::format(
                "http://{}:{}/proxy-{}", host, transport_env("LIFETCHTO_PEER_PORT"), scheme);
            auto pending = session->get(Request::from_url(target.as_str()).unwrap()).unwrap();
            EXPECT_EQ((co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                      "hello"_str);
            EXPECT_TRUE((co_await session->close()).is_ok());
        }
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
#endif
#if LITO_FEAT_QT
TEST(ProxyPeer, QtSocksUsesRemoteHostname) {
    if (! ::getenv("LIFETCHTO_PROXY_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = tls_backend(2,
                                   engine.resources(),
                                   peer_trust(),
                                   proxy_profile("LIFETCHTO_SOCKS_PORT", "socks5h"_str))
                           .unwrap();
        auto session = Session::from_http_backend(engine.handle(),
                                                  h::BackendBinding::Owned(rstd::move(backend)))
                           .unwrap();
        auto pending =
            session
                ->get(Request::from_url(rstd::format("http://origin.test:{}/proxy-qt-socks",
                                                     transport_env("LIFETCHTO_PEER_PORT"))
                                            .as_str())
                          .unwrap())
                .unwrap();
        EXPECT_EQ((co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                  "hello"_str);
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
#endif
