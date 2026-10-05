#include <rstd/test/gtest.hpp>
#include <cstdlib>
import ncrequest;
import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::async::block_on;
using rstd::bytes::Bytes;
using namespace ncrequest;

static auto open_upgrade(ref<str> path, UpgradeOptions options = {}) {
    auto* socket   = ::getenv("NCREQUEST_UPGRADE_SOCKET");
    auto  endpoint = Endpoint::unix_socket(
                         rstd::path::PathBuf::from(
                             rstd::ffi::CStr::from_ptr(socket ? socket : "/missing/upgrade-test")
                                 .to_str()
                                 .unwrap()))
                         .unwrap();
    auto  request =
        lihttpto::UpgradeRequest::make(lihttpto::Method::parse("POST"_str).unwrap(),
                                       path,
                                       "localhost"_str,
                                       lihttpto::UpgradeProtocol::make("tcp"_str).unwrap())
            .unwrap();
    return UpgradeConnection::open(rstd::move(endpoint), rstd::move(request), options);
}

TEST(HttpUpgrade, PreservesOutputAndHalfClose) {
    for (auto path :
         array<ref<str>, 3> { "/coalesced"_str, "/fragmented"_str, "/informational"_str }) {
        auto opened = block_on(open_upgrade(path));
        ASSERT_TRUE(opened.is_ok());
        auto wire = rstd::move(opened).unwrap();
        EXPECT_TRUE(wire.response().status.value() == u16(101));
        ASSERT_TRUE(block_on(wire.write(Bytes::copy_from_slice("input"_str.as_bytes()))).is_ok());
        ASSERT_TRUE(block_on(wire.close_input()).is_ok());
        String output;
        for (;;) {
            auto chunk = block_on(wire.read());
            ASSERT_TRUE(chunk.is_ok());
            if (chunk->is_empty()) break;
            for (auto byte : chunk->as_slice()) output.push_ascii(u8(byte));
        }
        EXPECT_TRUE(output.as_str() == "first:tail-after-eof"_str);
    }
}

TEST(HttpUpgrade, RejectsInvalidResponseAndLimits) {
    for (auto path : array<ref<str>, 4> {
             "/denied"_str, "/wrong-protocol"_str, "/truncated"_str, "/oversized"_str }) {
        auto result = block_on(open_upgrade(path));
        ASSERT_TRUE(result.is_err());
        auto error = rstd::move(result).unwrap_err();
        if (path == "/denied"_str) {
            ASSERT_TRUE(error.response.is_some());
            EXPECT_TRUE(error.response->status.value() == u16(403));
            EXPECT_TRUE(*error.protocol == lihttpto::UpgradeError::RejectedStatus);
            EXPECT_TRUE(error.body_complete);
            EXPECT_TRUE(error.error_body.as_slice() == "{}"_str.as_bytes());
        }
        if (path == "/wrong-protocol"_str)
            EXPECT_TRUE(*error.protocol == lihttpto::UpgradeError::InvalidProtocol);
        if (path == "/truncated"_str) EXPECT_TRUE(error.transport->is_Protocol());
        if (path == "/oversized"_str) EXPECT_TRUE(error.parse->kind().is_HeaderTooLarge());
    }
    UpgradeOptions options;
    options.handshake_timeout = rstd::time::Duration::from_millis(u64(40));
    options.duplex.io_timeout = rstd::time::Duration::from_secs(u64(2));
    auto result               = block_on(open_upgrade("/slow"_str, options));
    ASSERT_TRUE(result.is_err());
    EXPECT_TRUE(result.unwrap_err().transport->is_Timeout());
}

TEST(HttpUpgrade, CollectsBoundedFailureBodies) {
    for (auto path : array<ref<str>, 2> { "/denied-chunked"_str, "/denied-eof"_str }) {
        auto result = block_on(open_upgrade(path));
        ASSERT_TRUE(result.is_err());
        auto error = rstd::move(result).unwrap_err();
        EXPECT_TRUE(error.response->status.value() == u16(403));
        EXPECT_TRUE(error.body_complete && error.body.is_none());
        EXPECT_TRUE(error.error_body.as_slice() == "{}"_str.as_bytes());
    }
    for (auto path : array<ref<str>, 6> { "/denied-large"_str,
                                          "/denied-truncated"_str,
                                          "/denied-ambiguous"_str,
                                          "/denied-bad-chunk"_str,
                                          "/denied-eof-large"_str,
                                          "/denied-chunk-large"_str }) {
        auto result = block_on(open_upgrade(path));
        ASSERT_TRUE(result.is_err());
        auto error = rstd::move(result).unwrap_err();
        EXPECT_TRUE(error.response->status.value() == u16(403));
        EXPECT_TRUE(! error.body_complete && error.body.is_some());
        EXPECT_TRUE(error.error_body.len() <= usize(65536));
    }
    UpgradeOptions options;
    options.handshake_timeout = rstd::time::Duration::from_millis(u64(40));
    auto slow                 = block_on(open_upgrade("/denied-slow"_str, options));
    ASSERT_TRUE(slow.is_err());
    auto error = rstd::move(slow).unwrap_err();
    ASSERT_TRUE(error.response.is_some());
    EXPECT_TRUE(error.response->status.value() == u16(403));
    EXPECT_TRUE(! error.body_complete && error.transport->is_Timeout());
    auto endless = block_on(open_upgrade("/informational-loop"_str));
    ASSERT_TRUE(endless.is_err());
    EXPECT_TRUE(*endless.unwrap_err().protocol == lihttpto::UpgradeError::TooLarge);
    options              = {};
    options.header_bytes = usize(90);
    auto aggregate       = block_on(open_upgrade("/informational-limit"_str, options));
    ASSERT_TRUE(aggregate.is_err());
    EXPECT_TRUE(aggregate.unwrap_err().parse->kind().is_HeaderTooLarge());
}
