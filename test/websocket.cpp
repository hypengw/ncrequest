#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>

import ncrequest;
import rstd.cppstd;

namespace
{

using namespace rstd::literals;

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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

} // namespace

TEST(websocket, ConstructDisconnected) {
    auto client = ncrequest::WebSocketClient {};
    EXPECT_FALSE(client.is_connected());
    client.send("ignored while disconnected"_str);
    client.disconnect();
}

TEST(websocket, LocalEchoText) {
    auto url = local_ws_url();
    if (url.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_WS_URL is not set";
    }

    auto client = ncrequest::WebSocketClient {};

    std::promise<std::string> message_promise;
    auto                      message = message_promise.get_future();
    std::atomic_bool          got_message { false };

    std::promise<std::string> error_promise;
    auto                      error = error_promise.get_future();
    std::atomic_bool          got_error { false };
    std::promise<void>        disconnected_promise;
    auto                      disconnected = disconnected_promise.get_future();

    client.set_on_disconnected_callback([&disconnected_promise] {
        disconnected_promise.set_value();
    });

    client.set_on_message_callback(
        [&message_promise, &got_message](rstd::slice<rstd::u8> data, bool) {
            if (got_message.exchange(true)) return;

            std::string out(reinterpret_cast<const char*>(data.as_raw_ptr()),
                            data.len().to_primitive());
            message_promise.set_value(std::move(out));
        });
    client.set_on_error_callback([&error_promise, &got_error](rstd::ref<rstd::str> data) {
        if (got_error.exchange(true)) return;

        std::string out(reinterpret_cast<const char*>(data.data()), data.size().to_primitive());
        error_promise.set_value(std::move(out));
    });

    auto connected = rstd::async::block_on(
        client.connect(rstd::move(rstd::cppstd::as_str(url)).unwrap()));
    ASSERT_TRUE(connected.is_ok());
    ASSERT_TRUE(rstd::move(connected).unwrap());
    EXPECT_TRUE(client.is_connected());

    client.send("curl websocket payload"_str);
    ASSERT_TRUE(wait_future(message, std::chrono::seconds(5)))
        << (wait_future(error, std::chrono::milliseconds(0)) ? error.get() : "message timed out");
    EXPECT_EQ(message.get(), "curl websocket payload");

    client.disconnect();
    ASSERT_TRUE(wait_future(disconnected, std::chrono::seconds(5)));
    EXPECT_FALSE(client.is_connected());
}
