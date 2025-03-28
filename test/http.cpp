#include <format>
#include <gtest/gtest.h>
#include <asio/thread_pool.hpp>
#include <asio/posix/stream_descriptor.hpp>

import ncrequest;
import ncrequest.event;

TEST(http, BasicTest) {
    asio::thread_pool pool(1);

    EXPECT_TRUE(true);
}