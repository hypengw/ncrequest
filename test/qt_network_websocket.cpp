#if defined(LITO_FEAT_QT)
#    include <chrono>
#    include <cstdlib>
#    include <future>
#    include <optional>
#    include <string_view>
#    include <utility>
#    include <rstd/test/gtest.hpp>
#    include <QCoreApplication>
#    include <QEventLoop>
#    include <thread>

import ncrequest.qt_network;

using namespace rstd::prelude;
using namespace rstd::literals;
using ncrequest::qt_network::WebSocketClient;
using rstd::async::block_on;
using rstd::async::Completion;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::this_thread::sleep_for;

namespace
{

auto local_ws_url() -> std::string {
    auto* value = std::getenv("NCREQUEST_TEST_WS_URL");
    if (value == nullptr || *value == '\0') return {};
    return value;
}

template<typename T>
auto wait_future(std::future<T>& future, milliseconds timeout) -> bool {
    auto const deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        if (future.wait_for(milliseconds(0)) == std::future_status::ready) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        sleep_for(milliseconds(1));
    }
    return future.wait_for(milliseconds(0)) == std::future_status::ready;
}

auto wait_completion(Completion<bool> completion) {
    using Output = Completion<bool>::Output;

    QEventLoop            loop;
    std::optional<Output> output;
    auto worker = std::thread([&loop, &output, completion = rstd::move(completion)]() mutable {
        output.emplace(block_on(rstd::move(completion)));
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
    auto client = WebSocketClient {};
    EXPECT_FALSE(client.is_connected());
    client.send("ignored while disconnected"_str);
    client.disconnect();
}

TEST(qt_network_websocket, LocalEchoText) {
    auto url = local_ws_url();
    if (url.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_WS_URL is not set";
    }

    auto client = WebSocketClient {};

    std::promise<std::string> message_promise;
    auto                      message = message_promise.get_future();
    std::promise<void>        disconnected_promise;
    auto                      disconnected = disconnected_promise.get_future();
    client.set_on_disconnected_callback([&disconnected_promise] {
        disconnected_promise.set_value();
    });
    client.set_on_message_callback([&message_promise](slice<u8> data, bool) {
        std::string out(reinterpret_cast<const char*>(data.as_raw_ptr()),
                        data.len().to_primitive());
        message_promise.set_value(std::move(out));
    });

    auto connected = wait_completion(client.connect(
        rstd::str_::from_utf8(
            slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(url.data()), usize(url.size())))
            .unwrap()));
    ASSERT_TRUE(connected.is_ok());
    ASSERT_TRUE(rstd::move(connected).unwrap());
    EXPECT_TRUE(client.is_connected());

    client.send("qt websocket payload"_str);
    ASSERT_TRUE(wait_future(message, seconds(5)));
    EXPECT_EQ(message.get(), "qt websocket payload");

    client.disconnect();
    ASSERT_TRUE(wait_future(disconnected, seconds(5)));
    EXPECT_FALSE(client.is_connected());
}
#endif
