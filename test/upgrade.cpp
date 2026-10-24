#include <rstd/test/gtest.hpp>
#include <stdlib.h>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace h  = lifetchto::http;
template<class F>
void with_upgrade_backends(F test) {
    if (! ::getenv("NCREQUEST_UPGRADE_SOCKET")) GTEST_SKIP();
    for (int mode {}; mode < (LITO_FEAT_CURL ? 2 : 1); ++mode) {
        auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
        auto task    = [&]() -> coro<empty> {
            auto engine  = lf::Engine::make(runtime.handle()).unwrap();
            auto backend = [&]() -> Arc<rstd::dyn<h::ExchangeBackend>> {
#if LITO_FEAT_CURL
                if (mode == 1)
                    return client::curl::Backend::make(engine.resources()).unwrap().backend();
#endif
                return h::NativeBackend::make(engine.resources()).unwrap().backend();
            }();
            auto session = Session::from_http_backend(engine.handle(),
                                                      h::BackendBinding::Owned(rstd::move(backend)))
                               .unwrap();
            co_await test(session.clone());
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
            co_return empty {};
        };
        runtime.block_on(task());
    }
}
auto open_upgrade(Arc<Session> session, ref<str> path, UpgradeOptions options = {})
    -> PendingUpgrade {
    auto url =
        lihttpto::Url::parse_http(rstd::format("http://localhost{}", path).as_str()).unwrap();
    auto socket = rstd::ffi::CStr::from_ptr(::getenv("NCREQUEST_UPGRADE_SOCKET")).to_str().unwrap();
    h::RouteChoice route { Some(Arc<h::Route>::make(
        h::Route::unix_socket(url, Bytes::copy_from_slice(socket.as_bytes())).unwrap())) };
    auto request = UpgradeRequest::make({ rstd::move(url), rstd::move(route) },
                                        lihttpto::Method::parse("POST"_str).unwrap(),
                                        lihttpto::UpgradeProtocol::make("tcp"_str).unwrap())
                       .unwrap();
    return session->upgrade(rstd::move(request), options).unwrap();
}
TEST(HttpUpgrade, PreservesOutputAndHalfClose) {
    with_upgrade_backends([](Arc<Session> session) -> coro<empty> {
        for (auto path :
             array<ref<str>, 3> { "/coalesced"_str, "/fragmented"_str, "/informational"_str }) {
            auto pending = open_upgrade(session.clone(), path);
            auto opened  = co_await pending.opened();
            EXPECT_TRUE(opened.is_ok());
            if (opened.is_err()) continue;
            auto wire = rstd::move(opened).unwrap();
            EXPECT_EQ(wire.response().status.value(), u16(101));
            EXPECT_TRUE(
                (co_await wire.write(Bytes::copy_from_slice("input"_str.as_bytes()))).is_ok());
            EXPECT_TRUE((co_await wire.close_input()).is_ok());
            Vec<u8> output;
            for (;;) {
                auto next = co_await wire.read();
                EXPECT_TRUE(next.is_ok());
                if (next.is_err() || next->is_empty()) break;
                output.extend_from_slice(next->as_slice());
            }
            EXPECT_TRUE(output.as_slice() == "first:tail-after-eof"_str.as_bytes());
            EXPECT_TRUE(wire.finish().is_ok());
            auto report = co_await wire.completion().wait();
            EXPECT_TRUE(report->logical.result.is_ok());
            EXPECT_TRUE(report->cleanup.is_ok());
        }
        co_return empty {};
    });
}
TEST(HttpUpgrade, RejectsInvalidResponseAndLimits) {
    with_upgrade_backends([](Arc<Session> session) -> coro<empty> {
        for (auto path : array<ref<str>, 4> {
                 "/denied"_str, "/wrong-protocol"_str, "/truncated"_str, "/oversized"_str }) {
            auto pending = open_upgrade(session.clone(), path);
            auto result  = co_await pending.opened();
            EXPECT_TRUE(result.is_err());
            if (result.is_ok()) continue;
            auto error = rstd::move(result).unwrap_err();
            EXPECT_TRUE(error.report.is_some());
            EXPECT_TRUE((*error.report)->cleanup.is_ok());
            if (path == "/denied"_str) {
                EXPECT_EQ(error.response->status.value(), u16(403));
                EXPECT_TRUE(*error.protocol == lihttpto::UpgradeError::RejectedStatus);
                EXPECT_TRUE(error.body_complete);
                EXPECT_TRUE(error.error_body.as_slice() == "{}"_str.as_bytes());
            }
            if (path == "/wrong-protocol"_str)
                EXPECT_TRUE(*error.protocol == lihttpto::UpgradeError::InvalidProtocol);
            if (path == "/truncated"_str) EXPECT_TRUE(error.transport->is_Protocol());
            if (path == "/oversized"_str) EXPECT_TRUE(error.parse->kind().is_HeaderTooLarge());
        }
        UpgradeOptions options;
        options.handshake_timeout = rstd::time::Duration::from_millis(u64(40));
        auto pending              = open_upgrade(session.clone(), "/slow"_str, options);
        auto result               = co_await pending.opened();
        EXPECT_TRUE(result.is_err());
        EXPECT_TRUE(result.unwrap_err().transport->is_Timeout());
        co_return empty {};
    });
}
TEST(HttpUpgrade, CollectsBoundedFailureBodies) {
    with_upgrade_backends([](Arc<Session> session) -> coro<empty> {
        for (auto path : array<ref<str>, 2> { "/denied-chunked"_str, "/denied-eof"_str }) {
            auto pending = open_upgrade(session.clone(), path);
            auto result  = co_await pending.opened();
            EXPECT_TRUE(result.is_err());
            if (result.is_ok()) continue;
            auto error = rstd::move(result).unwrap_err();
            EXPECT_EQ(error.response->status.value(), u16(403));
            EXPECT_TRUE(error.body_complete && error.body.is_none());
            EXPECT_TRUE(error.error_body.as_slice() == "{}"_str.as_bytes());
        }
        for (auto path : array<ref<str>, 6> { "/denied-large"_str,
                                              "/denied-truncated"_str,
                                              "/denied-ambiguous"_str,
                                              "/denied-bad-chunk"_str,
                                              "/denied-eof-large"_str,
                                              "/denied-chunk-large"_str }) {
            auto pending = open_upgrade(session.clone(), path);
            auto result  = co_await pending.opened();
            EXPECT_TRUE(result.is_err());
            if (result.is_ok()) continue;
            auto error = rstd::move(result).unwrap_err();
            EXPECT_EQ(error.response->status.value(), u16(403));
            EXPECT_TRUE(! error.body_complete && error.body.is_some());
            EXPECT_TRUE(error.error_body.len() <= usize(65536));
        }
        UpgradeOptions options;
        options.handshake_timeout = rstd::time::Duration::from_millis(u64(40));
        auto pending              = open_upgrade(session.clone(), "/denied-slow"_str, options);
        auto slow                 = co_await pending.opened();
        EXPECT_TRUE(slow.is_err());
        auto error = rstd::move(slow).unwrap_err();
        EXPECT_EQ(error.response->status.value(), u16(403));
        EXPECT_TRUE(! error.body_complete && error.transport->is_Timeout());
        auto endless = open_upgrade(session.clone(), "/informational-loop"_str);
        auto result  = co_await endless.opened();
        EXPECT_TRUE(result.is_err());
        EXPECT_TRUE(*result.unwrap_err().protocol == lihttpto::UpgradeError::TooLarge);
        options              = {};
        options.header_bytes = usize(90);
        auto aggregate       = open_upgrade(session.clone(), "/informational-limit"_str, options);
        auto limited         = co_await aggregate.opened();
        EXPECT_TRUE(limited.is_err());
        EXPECT_TRUE(limited.unwrap_err().parse->kind().is_HeaderTooLarge());
        co_return empty {};
    });
}
