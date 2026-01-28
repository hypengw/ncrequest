#include <format>
#include <gtest/gtest.h>
#include <asio/thread_pool.hpp>
#include <asio/co_spawn.hpp>
#include <asio/awaitable.hpp>
#include <asio/read.hpp>
#include <asio/streambuf.hpp>
#include <asio/as_tuple.hpp>
#include <asio/detached.hpp>
#include <asio/posix/stream_descriptor.hpp>

import ncrequest;
import ncrequest.event;

TEST(http, BasicTest) {
    asio::thread_pool pool(1);
    {
        auto session = ncrequest::Session::make(pool.get_executor());

        asio::co_spawn(
            pool.get_executor(),
            [session]() -> asio::awaitable<void> {
                auto req = ncrequest::Request { "https://www.baidu.com" };
                auto rsp = (co_await session->get(req)).unwrap();

                asio::streambuf buf;
                [[maybe_unused]] auto [ec, size] = co_await asio::async_read(
                    *rsp, buf, asio::transfer_all(), asio::as_tuple(asio::use_awaitable));
            },
            asio::detached);
    }
    pool.join();

    EXPECT_TRUE(true);
}