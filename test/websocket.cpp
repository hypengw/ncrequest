#include <format>
#include <gtest/gtest.h>
#include <asio/thread_pool.hpp>

import ncrequest;
import ncrequest.event;

TEST(websocket, BasicTest) {
    ncrequest::global_init();

    asio::thread_pool          pool(1);
    ncrequest::WebSocketClient ws(ncrequest::event::create(pool));

    ws.set_on_error_callback([](std::string_view m) {
        printf("error: %s\n", std::string(m).c_str());
    });
    ws.set_on_message_callback([](std::span<const std::byte> data, bool last) {
        printf("recv: %.*s\n", (int)data.size(), (char*)data.data());
    });
    printf("connecting\n");
    EXPECT_TRUE(ws.connect("ws://127.0.0.3:46543"));
    int i = 0;
    while (i++ < 10) {
        ws.send(std::format("ok {}", i));
    };
    sleep(2);
    ws.disconnect();
    pool.stop();
    pool.join();
    EXPECT_TRUE(true); // Replace with actual tests
}
