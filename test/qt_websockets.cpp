#include <chrono>
#include <cstdlib>
#include <future>
#include <span>
#include <utility>
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <thread>

import ncrequest;
import ncrequest.qt_websockets;

namespace
{

auto local_ws_url() -> std::string {
    auto* value = std::getenv("NCREQUEST_TEST_WS_URL");
    if (value == nullptr || *value == '\0') return {};
    return value;
}

template<typename T>
auto wait_future(std::future<T>& future, std::chrono::milliseconds timeout) -> bool {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

} // namespace

TEST(qt_websockets, ConstructDisconnected) {
    auto client = ncrequest::qt_websockets::WebSocketClient {};
    EXPECT_FALSE(client.is_connected());
    client.send("ignored while disconnected");
    client.disconnect();
}

TEST(qt_websockets, LocalEchoText) {
    auto url = local_ws_url();
    if (url.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_WS_URL is not set";
    }

    auto client = ncrequest::qt_websockets::WebSocketClient {};

    std::promise<std::string> message_promise;
    auto                      message = message_promise.get_future();
    client.set_on_message_callback([&message_promise](std::span<const rstd::byte> data, bool) {
        std::string out(reinterpret_cast<const char*>(data.data()), data.size());
        message_promise.set_value(std::move(out));
    });

    auto connected = client.connect(url);
    ASSERT_TRUE(wait_future(connected, std::chrono::seconds(5)));
    ASSERT_TRUE(connected.get());
    EXPECT_TRUE(client.is_connected());

    client.send("qt websocket payload");
    ASSERT_TRUE(wait_future(message, std::chrono::seconds(5)));
    EXPECT_EQ(message.get(), "qt websocket payload");

    client.disconnect();
}
