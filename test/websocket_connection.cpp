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

static auto serve_driver(rstd::net::TcpListener& listener, bool cancel) -> rstd::async::coro<bool> {
    auto accepted = co_await listener.accept();
    if (accepted.is_err()) co_return failure(__LINE__);
    auto                 pair = rstd::move(accepted).unwrap();
    lihttpto::Connection http(rstd::move(pair.get<0>()));
    auto                 request = co_await http.read_request();
    if (request.is_err() || request->is_none()) co_return failure(__LINE__);
    auto head = request->take().unwrap();
    auto auth = head.headers.get_unique_text("authorization"_str);
    if (auth.is_err() || auth->is_none() || **auth != "Bearer interop"_str)
        co_return failure(__LINE__);
    auto handshake = liwebsocketo::ServerHandshake::parse(head, Some("fern.test"_str));
    if (handshake.is_err()) co_return failure(__LINE__);
    auto opened = co_await liwebsocketo::ServerConnection::accept(http, *handshake);
    if (opened.is_err()) co_return failure(__LINE__);
    auto connection = rstd::move(opened).unwrap();
    if (cancel) {
        auto pending = connection.receive();
        connection.cancel();
        auto result = co_await rstd::move(pending);
        bool ok =
            result.is_ok() && result->is_err() && result->unwrap_err() == WebSocketError::Cancelled;
        co_await connection.shutdown();
        co_return ok;
    }
    auto sent = co_await connection.send(
        { liwebsocketo::Kind::Ping, Bytes::copy_from_slice("check"_str.as_bytes()) });
    if (sent.is_err() || sent->is_err()) co_return failure(__LINE__);
    auto pong = co_await connection.receive();
    if (pong.is_err() || pong->is_err()) co_return failure(__LINE__);
    auto pong_message = rstd::move(*pong).unwrap();
    if (pong_message.kind != liwebsocketo::Kind::Pong ||
        pong_message.data.as_slice() != "check"_str.as_bytes())
        co_return failure(__LINE__);
    auto message = co_await connection.receive();
    if (message.is_err() || message->is_err()) co_return failure(__LINE__);
    auto echoed = co_await connection.send(rstd::move(*message).unwrap());
    if (echoed.is_err() || echoed->is_err()) co_return failure(__LINE__);
    auto closed = co_await connection.close();
    bool ok     = closed.is_ok() && closed->is_ok();
    co_await connection.shutdown();
    co_return ok;
}
static auto interop(bool cancel) -> rstd::async::coro<bool> {
    auto listener =
        rstd::net::TcpListener::bind(rstd::net::SocketAddr::ipv4_loopback(u16())).unwrap();
    auto address = listener.local_addr().unwrap();
    auto server =
        rstd::async::AbortOnDropHandle(rstd::async::spawn(serve_driver(listener, cancel)));
    auto              url = rstd::format("http://127.0.0.1:{}/worker", address.port());
    lihttpto::Headers headers;
    headers.add("Authorization"_str, "Bearer interop"_str).unwrap();
    Vec<String> protocols;
    protocols.push(String::make("fern.test"_str));
    auto opened =
        co_await WebSocketConnection::connect(lihttpto::Url::parse_http(url.as_str()).unwrap(),
                                              rstd::move(headers),
                                              rstd::move(protocols));
    if (opened.is_err()) co_return failure(__LINE__);
    auto client = rstd::move(opened).unwrap();
    if (! cancel) {
        // Receiving Ping does not require an application receive call to produce Pong.
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(20)));
        auto sent = co_await client.send(
            { liwebsocketo::Kind::Text, Bytes::copy_from_slice("roundtrip"_str.as_bytes()) });
        if (sent.is_err() || sent->is_err()) co_return failure(__LINE__);
        auto reply = co_await client.receive();
        if (reply.is_err() || reply->is_err() ||
            reply->unwrap().data.as_slice() != "roundtrip"_str.as_bytes())
            co_return failure(__LINE__);
        auto closed = co_await client.receive();
        if (closed.is_err() || closed->is_ok() || closed->unwrap_err() != WebSocketError::Closed)
            co_return failure(__LINE__);
    }
    auto done = co_await rstd::future::by_ref(server);
    co_await client.shutdown();
    co_return done.is_ok() && *done;
}
static void run_interop(bool cancel) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = rstd::async::AbortOnDropHandle(runtime.spawn(interop(cancel)));
    auto result  = runtime.block_on(
        rstd::async::timeout(rstd::future::by_ref(task), rstd::time::Duration::from_secs(u64(5))));
    ASSERT_TRUE(result.is_ok() && result->is_ok());
    EXPECT_TRUE(**result);
}
TEST(WebSocketInterop, ServerAndClientDrivers) { run_interop(false); }
TEST(WebSocketInterop, ServerCancelsPendingRead) { run_interop(true); }
#endif
