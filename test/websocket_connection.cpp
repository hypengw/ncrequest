#if ! defined(LITO_FEAT_QT) && defined(__linux__)
#    include <rstd/test/gtest.hpp>
#    include <cstdlib>
#    include <cstdio>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
using rstd::bytes::Bytes;
static auto failure(int line) -> bool {
    std::fprintf(stderr, "websocket check failed at line %d\n", line);
    return false;
}

static auto scenario(unsigned mode) -> rstd::async::coro<bool> {
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
    auto opened =
        co_await WebSocketConnection::connect(lihttpto::Url::parse_http(address.as_str()).unwrap(),
                                              rstd::move(headers),
                                              rstd::move(protocols),
                                              options);
    if (mode == 6) co_return opened.is_err() && opened.unwrap_err().handshake.is_some();
    if (opened.is_err()) co_return failure(__LINE__);
    auto connection = rstd::move(opened).unwrap();
    bool ok         = false;
    if (mode == 0) {
        auto ready = co_await connection.receive();
        if (ready.is_err() || ready->is_err() ||
            ready->unwrap().data.as_slice() != "ready"_str.as_bytes())
            co_return failure(__LINE__);
        Vec<u8> data;
        data.resize(usize(65536), u8('x'));
        auto sent = connection.send(
            { liwebsocketo::Kind::Binary, Bytes::copy_from_slice(data.as_slice()) });
        auto busy = co_await connection.send({ liwebsocketo::Kind::Text, {} });
        if (busy.is_err() || busy->is_ok() || busy->unwrap_err() != WebSocketError::Busy)
            co_return failure(__LINE__);
        auto written = co_await rstd::move(sent);
        if (written.is_err() || written->is_err()) co_return failure(__LINE__);
        auto echo = co_await connection.receive();
        if (echo.is_err() || echo->is_err() || echo->unwrap().data.as_slice() != data.as_slice())
            co_return failure(__LINE__);
        auto closed = co_await connection.close();
        ok          = closed.is_ok() && closed->is_ok();
    } else if (mode == 1) {
        auto closed = co_await connection.close();
        ok = closed.is_ok() && closed->is_err() && closed->unwrap_err() == WebSocketError::Timeout;
    } else if (mode == 2) {
        auto waiting = connection.receive();
        connection.cancel();
        auto done = co_await rstd::move(waiting);
        ok = done.is_ok() && done->is_err() && done->unwrap_err() == WebSocketError::Cancelled;
    } else if (mode == 3) {
        auto waiting = connection.receive();
        waiting.close();
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        auto done = co_await connection.receive();
        ok = done.is_ok() && done->is_err() && done->unwrap_err() == WebSocketError::Cancelled;
    } else if (mode == 4 || mode == 7) {
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        auto done = co_await connection.receive();
        ok = done.is_ok() && done->is_err() && done->unwrap_err() == WebSocketError::Overflow;
    } else if (mode == 5) {
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        auto done = co_await connection.receive();
        if (done.is_err() || done->is_err() ||
            done->unwrap().data.as_slice() != "last"_str.as_bytes())
            co_return failure(__LINE__);
        auto closed = co_await connection.receive();
        ok = closed.is_ok() && closed->is_err() && closed->unwrap_err() == WebSocketError::Closed;
    } else if (mode == 8) {
        auto sending = connection.send(
            { liwebsocketo::Kind::Text, Bytes::copy_from_slice("discard"_str.as_bytes()) });
        sending.close();
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(40)));
        auto done = co_await connection.receive();
        ok = done.is_ok() && done->is_err() && done->unwrap_err() == WebSocketError::Cancelled;
    }
    co_await connection.shutdown();
    co_return ok;
}
static void run(unsigned mode) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = rstd::async::AbortOnDropHandle(runtime.spawn(scenario(mode)));
    auto result  = runtime.block_on(
        rstd::async::timeout(rstd::future::by_ref(task), rstd::time::Duration::from_secs(u64(5))));
    ASSERT_TRUE(result.is_ok() && result->is_ok());
    EXPECT_TRUE(**result);
}
TEST(WebSocketConnection, AuthenticationPongLargeFrameAndClose) { run(0); }
TEST(WebSocketConnection, CloseDeadlineWakesIdleReader) { run(1); }
TEST(WebSocketConnection, CancelCompletesReceiver) { run(2); }
TEST(WebSocketConnection, DroppedReceiverCancelsConnection) { run(3); }
TEST(WebSocketConnection, ReceiveQueueIsBounded) { run(4); }
TEST(WebSocketConnection, PeerClosePreservesQueuedMessage) { run(5); }
TEST(WebSocketConnection, RejectsWrongAccept) { run(6); }
TEST(WebSocketConnection, ReceiveBytesAreBounded) { run(7); }
TEST(WebSocketConnection, DroppedSenderCancelsConnection) { run(8); }

#endif
