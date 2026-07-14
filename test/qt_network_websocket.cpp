#include <chrono>
#include <cstdlib>
#include <future>
#include <optional>
#include <utility>
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <thread>

import ncrequest.qt_network;

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

auto wait_completion(rstd::async::Completion<bool> completion) {
    using Output = rstd::async::Completion<bool>::Output;

    QEventLoop            loop;
    std::optional<Output> output;
    auto worker = std::thread([&loop, &output, completion = rstd::move(completion)]() mutable {
        output.emplace(rstd::async::block_on(rstd::move(completion)));
        (void)QMetaObject::invokeMethod(
            &loop,
            [&loop] {
                loop.quit();
            },
            Qt::QueuedConnection);
    });

    loop.exec();
    worker.join();
    return rstd::move(*output);
}

} // namespace

TEST(qt_network_websocket, ConstructDisconnected) {
    auto client = ncrequest::qt_network::WebSocketClient {};
    EXPECT_FALSE(client.is_connected());
    client.send("ignored while disconnected");
    client.disconnect();
}

TEST(qt_network_websocket, LocalEchoText) {
    auto url = local_ws_url();
    if (url.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_WS_URL is not set";
    }

    auto client = ncrequest::qt_network::WebSocketClient {};

    std::promise<std::string> message_promise;
    auto                      message = message_promise.get_future();
    std::promise<void>        disconnected_promise;
    auto                      disconnected = disconnected_promise.get_future();
    client.set_on_disconnected_callback([&disconnected_promise] {
        disconnected_promise.set_value();
    });
    client.set_on_message_callback([&message_promise](rstd::slice<rstd::byte> data, bool) {
        std::string out(reinterpret_cast<const char*>(data.as_raw_ptr()), data.len());
        message_promise.set_value(std::move(out));
    });

    auto connected = wait_completion(client.connect(url));
    ASSERT_TRUE(connected.is_ok());
    ASSERT_TRUE(rstd::move(connected).unwrap());
    EXPECT_TRUE(client.is_connected());

    client.send("qt websocket payload");
    ASSERT_TRUE(wait_future(message, std::chrono::seconds(5)));
    EXPECT_EQ(message.get(), "qt websocket payload");

    client.disconnect();
    ASSERT_TRUE(wait_future(disconnected, std::chrono::seconds(5)));
    EXPECT_FALSE(client.is_connected());
}
