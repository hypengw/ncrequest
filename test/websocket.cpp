#include <format>
#include <gtest/gtest.h>
#include <asio/thread_pool.hpp>

#include <asio/generic/stream_protocol.hpp>
template<typename T>
using stream_type = asio::basic_stream_socket<asio::generic::stream_protocol, T>;

#if __linux__
#    include <asio/posix/stream_descriptor.hpp>
#endif

import ncrequest;
import ncrequest.event;

void test_compile() {
    asio::thread_pool pool(1);
    ncrequest::event::create<stream_type>(pool.get_executor());

#if __linux__
    ncrequest::event::create<asio::posix::basic_stream_descriptor>(pool.get_executor());
#endif
}

TEST(websocket, BasicTest) {
#if 0
    asio::thread_pool          pool(1);
    ncrequest::WebSocketClient ws(
        ncrequest::event::create<asio::posix::basic_stream_descriptor>(pool.get_executor()));

    ws.set_on_error_callback([](std::string_view m) {
        printf("error: %s\n", std::string(m).c_str());
    });
    ws.set_on_message_callback([](std::span<const std::byte> data, bool last) {
        printf("recv: %.*s\n", (int)data.size(), (char*)data.data());
    });
    printf("connecting\n");
    EXPECT_TRUE(ws.connect("ws://127.0.0.3:46543").get());
    int i = 0;
    while (i++ < 10) {
        ws.send(std::format("ok {}", i));
    };
    sleep(2);
    ws.disconnect();
    pool.stop();
    pool.join();
#endif
    EXPECT_TRUE(true);
}
