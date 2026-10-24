#include <rstd/test/gtest.hpp>
#include <cstdlib>
#include <cstdio>
import ncrequest;
#include "ws_helpers.hpp"
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
using rstd::bytes::Bytes;
static auto failure(int line) -> bool {
    std::fprintf(stderr, "websocket check failed at line %d\n", line);
    return false;
}

static auto scenario(Arc<Session> session, unsigned mode) -> rstd::async::coro<bool> {
    auto base    = rstd::ffi::CStr::from_ptr(::getenv("NCREQUEST_WS_URL")).to_str().unwrap();
    auto address = rstd::format("{}/{}", base, mode);
    lihttpto::Headers headers;
    headers.add("Authorization"_str, "Bearer local-test"_str).unwrap();
    Vec<String> protocols;
    protocols.push(String::make("fern.test"_str));
    WebSocketOptions options;
    options.close_timeout = rstd::time::Duration::from_millis(u64(60));
    if (mode == 4) options.receive_messages = usize(1);
    if (mode == 7) options.receive_bytes = usize(1);
    if (mode == 18)
        options.upgrade.duplex.total_timeout =
            Some(lifetchto::http::TotalTimeout::after(rstd::time::Duration::from_millis(u64(100))));
    if (mode == 17) options.limits.message_bytes = usize(3);
    if (mode == 10) options.limits.frame_bytes = usize(32);
    if (mode == 11) {
        options.limits.frame_bytes   = usize(32);
        options.limits.message_bytes = usize(50);
    }
    auto opened = co_await WebSocketConnection::connect(
        session.clone(),
        { lihttpto::Url::parse_http(address.as_str()).unwrap(), {} },
        rstd::move(headers),
        rstd::move(protocols),
        options);
    if (mode == 6)
        co_return opened.is_err() && (opened.as_ref().unwrap_err().handshake.is_some() ||
                                      (! session->websocket_capabilities().handshake_response &&
                                       opened.as_ref().unwrap_err().transport.is_some()));
    if (opened.is_err()) co_return failure(__LINE__);
    auto connection = rstd::move(opened).unwrap();
    if (connection.protocol().is_none() || connection.protocol()->as_str() != "fern.test"_str)
        co_return failure(__LINE__);
    bool ok = false;
    if (mode == 0) {
        auto ready = co_await read_ws(connection);
        if (ready.is_err() || ready->data.as_slice() != "ready"_str.as_bytes())
            co_return failure(__LINE__);
        Vec<u8> data;
        data.resize(usize(65537), u8('x'));
        auto first =
            connection.send({ liwebsocketo::Kind::Binary, Bytes::copy_from_slice(data.as_slice()) })
                .unwrap();
        auto second =
            connection
                .send({ liwebsocketo::Kind::Text, Bytes::copy_from_slice("second"_str.as_bytes()) })
                .unwrap();
        if ((co_await first.written()).is_err() || (co_await second.written()).is_err())
            co_return failure(__LINE__);
        auto echo = co_await read_ws(connection);
        if (echo.is_err() || echo->data.as_slice() != data.as_slice()) co_return failure(__LINE__);
        echo = co_await read_ws(connection);
        if (echo.is_err() || echo->data.as_slice() != "second"_str.as_bytes())
            co_return failure(__LINE__);
        ok = (co_await connection.close()).is_ok();
    } else if (mode == 1) {
        auto closed = co_await connection.close();
        ok          = closed.is_err() && closed.as_ref().unwrap_err() == WebSocketError::Timeout;
    } else if (mode == 2) {
        auto pending = connection.receive();
        connection.cancel();
        auto done = co_await pending.message();
        ok        = done.is_err() && done.as_ref().unwrap_err() == WebSocketError::Cancelled;
    } else if (mode == 3) {
        {
            auto pending = connection.receive();
            pending.cancel();
        }
        auto pending = connection.receive();
        auto done    = co_await pending.message();
        ok           = done.is_err() && done.as_ref().unwrap_err() == WebSocketError::Cancelled;
    } else if (mode == 4 || mode == 7) {
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        auto pending = connection.receive();
        auto done    = co_await pending.message();
        auto code =
            mode == 4 ? WebSocketError::ReceiveMessagesFull : WebSocketError::ReceiveBytesFull;
        ok = done.is_err() && done.as_ref().unwrap_err() == code &&
             done.as_ref().unwrap_err().limit()->maximum == usize(1);
    } else if (mode == 19) {
        (void)co_await connection.completion().wait();
        if ((co_await connection.shutdown()).is_err()) co_return failure(__LINE__);
        auto pending  = connection.receive();
        auto received = co_await pending.message();
        ok = received.is_err() && received.as_ref().unwrap_err() == WebSocketError::Closed;
    } else if (mode == 5) {
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        {
            auto pending = connection.receive();
            auto done    = co_await pending.message();
            if (done.is_err() || done->data.as_slice() != "last"_str.as_bytes())
                co_return failure(__LINE__);
        }
        auto pending = connection.receive();
        auto closed  = co_await pending.message();
        ok           = closed.is_err() && closed.as_ref().unwrap_err() == WebSocketError::Closed;
    } else if (mode == 10 || mode == 11 || mode == 13 || mode == 14) {
        (void)co_await connection.completion().wait();
        auto pending = connection.receive();
        auto done    = co_await pending.message();
        auto code    = mode == 10   ? WebSocketError::FrameTooLarge
                       : mode == 11 ? WebSocketError::MessageTooLarge
                       : mode == 13 ? WebSocketError::ReceiveBytesFull
                                    : WebSocketError::ReceiveMessagesFull;
        if (! session->websocket_capabilities().receive_limit_details && (mode == 10 || mode == 11))
            code = WebSocketError::IncomingLimit;
        ok = done.is_err() && done.as_ref().unwrap_err() == code;
        if (ok && code != WebSocketError::IncomingLimit) {
            auto limit = done.as_ref().unwrap_err().limit();
            ok         = limit.is_some() && limit->observed > limit->maximum;
        }
    } else if (mode == 18) {
        (void)co_await connection.completion().wait();
        auto error = connection.failure();
        ok         = error.is_some() && *error == WebSocketError::Timeout;
    } else if (mode == 17) {
        (void)co_await connection.completion().wait();
        auto error = connection.failure();
        ok         = error.is_some();
        if (ok && *error == WebSocketError::IncomingLimit)
            ok = ! session->websocket_capabilities().receive_limit_details;
        else
            ok = ok && *error == WebSocketError::MessageTooLarge &&
                 error->limit()->maximum == usize(3) && error->limit()->observed == usize(4);
    } else if (mode == 16) {
        (void)co_await connection.completion().wait();
        auto error = connection.failure();
        ok         = error.is_some() && *error == WebSocketError::Protocol;
        if (session->websocket_capabilities().receive_limit_details)
            ok = ok && error->protocol_error() == Some(liwebsocketo::MessageError::Utf8);
    } else if (mode == 12) {
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        ok = true;
        for (int i {}; i < 11; ++i) {
            auto next = co_await read_ws(connection);
            if (next.is_err()) {
                ok = false;
                break;
            }
            if (i == 9)
                ok = ok && next->kind == liwebsocketo::Kind::Binary &&
                     next->data.len() == usize(262144);
            else if (i == 10)
                ok = ok && next->data.is_empty();
            else
                ok = ok && next->data.as_slice() == "state"_str.as_bytes();
        }
    } else if (mode == 15) {
        auto pending = connection.receive();
        auto first   = co_await pending.message();
        if (first.is_err()) co_return failure(__LINE__);
        (void)co_await connection.completion().wait();
        auto error = connection.failure();
        ok         = error.is_some() && *error == WebSocketError::ReceiveMessagesFull;
    } else if (mode == 8) {
        auto sending = connection
                           .send({ liwebsocketo::Kind::Text,
                                   Bytes::copy_from_slice("discard"_str.as_bytes()) })
                           .unwrap();
        sending.cancel();
        auto written = co_await sending.written();
        ok = written.is_err() && written.as_ref().unwrap_err() == WebSocketError::Cancelled &&
             connection.failure().is_none();
    }
    co_await connection.shutdown();
    co_return ok;
}
static void run(unsigned mode) {
    if (! ::getenv("NCREQUEST_WS_URL")) GTEST_SKIP();
    for (int backend_mode {}; backend_mode < 3; ++backend_mode) {
        if ((backend_mode == 1 && ! LITO_FEAT_CURL) || (backend_mode == 2 && ! LITO_FEAT_QT))
            continue;
        auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
        auto test    = [&]() -> coro<bool> {
            auto engine  = lifetchto::Engine::make(runtime.handle()).unwrap();
            auto session = [&]() -> Arc<Session> {
#if LITO_FEAT_QT
                if (backend_mode == 2)
                    return Session::from_backend(
                               engine.handle(),
                               client::qt_network::Backend::make(engine.resources())
                                   .unwrap()
                                   .owned_binding())
                        .unwrap();
#endif
                auto backend = [&]() -> Arc<rstd::dyn<lifetchto::http::ExchangeBackend>> {
#if LITO_FEAT_CURL
                    if (backend_mode == 1)
                        return client::curl::Backend::make(engine.resources()).unwrap().backend();
#endif
                    return lifetchto::http::NativeBackend::make(engine.resources())
                        .unwrap()
                        .backend();
                }();
                return Session::from_http_backend(
                           engine.handle(),
                           lifetchto::http::BackendBinding::Owned(rstd::move(backend)))
                    .unwrap();
            }();
            bool result = co_await scenario(session.clone(), mode);
            result      = (co_await session->close()).is_ok() && result;
            result      = engine.resources().usage().bytes == usize() && result;
            result      = (co_await engine.close()).is_ok() && result;
            co_return result;
        };
        auto task   = rstd::async::AbortOnDropHandle(runtime.spawn(test()));
        auto result = runtime.block_on(rstd::async::timeout(
            rstd::future::by_ref(task), rstd::time::Duration::from_secs(u64(5))));
        ASSERT_TRUE(result.is_ok() && result->is_ok());
        EXPECT_TRUE(**result);
    }
}
TEST(WebSocketConnection, AuthenticationPongLargeFrameAndClose) { run(0); }
TEST(WebSocketConnection, CloseDeadlineWakesIdleReader) { run(1); }
TEST(WebSocketConnection, CancelCompletesReceiver) { run(2); }
TEST(WebSocketConnection, DroppedReceiverCancelsConnection) { run(3); }
TEST(WebSocketConnection, ReceiveQueueIsBounded) { run(4); }
TEST(WebSocketConnection, PeerClosePreservesQueuedMessage) { run(5); }
TEST(WebSocketConnection, RejectsWrongAccept) { run(6); }
TEST(WebSocketConnection, ReceiveBytesAreBounded) { run(7); }
TEST(WebSocketConnection, CancelledQueuedSenderKeepsConnection) { run(8); }
TEST(WebSocketConnection, ShutdownDiscardsUnreadMessagesAfterPeerClose) { run(19); }
TEST(WebSocketConnection, TotalDeadlineRetainsTimeoutAfterHandshake) { run(18); }
TEST(WebSocketConnection, TextMessageBudgetCountsUtf8Bytes) { run(17); }
TEST(WebSocketConnection, ProtocolFailureRetainsDecoderCause) { run(16); }
TEST(WebSocketConnection, FrameLimitRetainsTypedFailure) { run(10); }
TEST(WebSocketConnection, FragmentedMessageLimitRetainsTypedFailure) { run(11); }
TEST(WebSocketConnection, MixedStateBurstLargeResponseAndEmptyMessage) { run(12); }
TEST(WebSocketConnection, LargeResponseBurstExceedsByteBudget) { run(13); }
TEST(WebSocketConnection, EmptyMessageBurstExceedsCountBudget) { run(14); }
TEST(WebSocketConnection, DeliveredReceiptStillCountsWhileItsOwnerIsRetained) { run(15); }

TEST(WebSocketConnection, RegisteredTaskQuotaFailureClosesNativeStream) {
    if (! ::getenv("NCREQUEST_WS_URL")) GTEST_SKIP();
    for (int mode {}; mode < (LITO_FEAT_CURL ? 2 : 1); ++mode) {
        auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
        auto test    = [&]() -> coro<empty> {
            lifetchto::EngineOptions limits;
            limits.limits.tasks = usize(3);
            auto engine         = lifetchto::Engine::make(runtime.handle(), limits).unwrap();
            auto backend        = [&]() -> Arc<rstd::dyn<lifetchto::http::ExchangeBackend>> {
#if LITO_FEAT_CURL
                if (mode == 1)
                    return client::curl::Backend::make(engine.resources()).unwrap().backend();
#endif
                return lifetchto::http::NativeBackend::make(engine.resources()).unwrap().backend();
            }();
            auto session =
                Session::from_http_backend(
                    engine.handle(), lifetchto::http::BackendBinding::Owned(rstd::move(backend)))
                    .unwrap();
            auto base = rstd::ffi::CStr::from_ptr(::getenv("NCREQUEST_WS_URL")).to_str().unwrap();
            auto url  = lihttpto::Url::parse_http(rstd::format("{}/2", base).as_str()).unwrap();
            lihttpto::Headers headers;
            headers.add("Authorization"_str, "Bearer local-test"_str).unwrap();
            Vec<String> protocols;
            protocols.push("fern.test"_Str);
            auto opened = co_await WebSocketConnection::connect(session.clone(),
                                                                { rstd::move(url), {} },
                                                                rstd::move(headers),
                                                                rstd::move(protocols));
            EXPECT_TRUE(opened.is_err());
            if (opened.is_err()) {
                auto error = rstd::move(opened).unwrap_err();
                EXPECT_TRUE(error.admission.is_some());
                EXPECT_TRUE(error.admission->is_Capacity());
                EXPECT_TRUE(error.report.is_some());
                EXPECT_TRUE((*error.report)->cleanup.is_ok());
            }
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_EQ(engine.resources().usage().tasks, usize());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
            co_return empty {};
        };
        runtime.block_on(test());
    }
}
