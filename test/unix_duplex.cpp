#include <rstd/test/gtest.hpp>
#include <cstdlib>
#include <dirent.h>
import ncrequest;
import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::async::block_on;
using rstd::bytes::Bytes;
using namespace ncrequest;

static auto endpoint() -> Endpoint {
    auto* path = ::getenv("NCREQUEST_DUPLEX_SOCKET");
    return Endpoint::unix_socket(rstd::path::PathBuf::from(
                                     rstd::ffi::CStr::from_ptr(path ? path : "/missing/duplex-test")
                                         .to_str()
                                         .unwrap()))
        .unwrap();
}

TEST(UnixDuplex, HalfCloseAndConcurrentReadWrite) {
    auto made = UnixDuplex::make(endpoint());
    ASSERT_TRUE(made.is_ok());
    auto wire = rstd::move(made).unwrap();
    ASSERT_TRUE(block_on(wire->connected()).unwrap().is_ok());
    auto read      = wire->read();
    auto duplicate = block_on(wire->read()).unwrap();
    ASSERT_TRUE(duplicate.is_err());
    EXPECT_TRUE(duplicate.unwrap_err().is_InvalidState());
    ASSERT_TRUE(
        block_on(wire->write(Bytes::copy_from_slice("echo"_str.as_bytes()))).unwrap().is_ok());
    ASSERT_TRUE(block_on(wire->close_input()).unwrap().is_ok());
    ASSERT_TRUE(block_on(wire->close_input()).unwrap().is_ok());
    EXPECT_TRUE(block_on(wire->write({})).unwrap().is_err());
    auto first = block_on(rstd::move(read)).unwrap();
    ASSERT_TRUE(first.is_ok());
    String output;
    for (auto byte : first->as_slice()) output.push_ascii(u8(byte));
    for (;;) {
        auto next = block_on(wire->read()).unwrap();
        ASSERT_TRUE(next.is_ok());
        if (next->is_empty()) break;
        for (auto byte : next->as_slice()) output.push_ascii(u8(byte));
    }
    EXPECT_TRUE(output.as_str() == "echo:after-eof"_str);
    EXPECT_TRUE(block_on(wire->read()).unwrap().unwrap().is_empty());
}

TEST(UnixDuplex, CancelAndTimeout) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        DuplexOptions options;
        options.io_timeout = rstd::time::Duration::from_millis(u64(80));
        auto wire          = UnixDuplex::make(endpoint(), options).unwrap();
        ASSERT_TRUE(block_on(wire->connected()).unwrap().is_ok());
        auto pending = wire->read();
        if (mode == 0) {
            wire->cancel();
            wire->cancel();
        }
        if (mode == 2) {
            pending.close();
            auto result =
                block_on(wire->write(Bytes::copy_from_slice("ignored"_str.as_bytes()))).unwrap();
            ASSERT_TRUE(result.is_err());
            EXPECT_TRUE(result.unwrap_err().is_Canceled());
            continue;
        }
        auto result = block_on(rstd::move(pending)).unwrap();
        ASSERT_TRUE(result.is_err());
        auto error = result.unwrap_err();
        EXPECT_TRUE(mode == 0 ? error.is_Canceled() : error.is_Timeout());
    }
}

static auto descriptor_count() -> int {
    auto* directory = ::opendir("/proc/self/fd");
    if (! directory) return -1;
    int count = 0;
    while (::readdir(directory)) ++count;
    ::closedir(directory);
    return count;
}

TEST(UnixDuplex, DestructionCancelsReadAndReleasesDescriptors) {
    auto destroy_pending = [] {
        auto wire = Some(UnixDuplex::make(endpoint()).unwrap());
        ASSERT_TRUE(block_on((*wire)->connected()).unwrap().is_ok());
        auto pending = (*wire)->read();
        wire         = None();
        auto result  = block_on(rstd::move(pending)).unwrap();
        ASSERT_TRUE(result.is_err());
        EXPECT_TRUE(result.unwrap_err().is_Canceled());
    };
    destroy_pending();
    auto before = descriptor_count();
    ASSERT_TRUE(before >= 0);
    for (unsigned i = 0; i < 20; ++i) destroy_pending();
    EXPECT_TRUE(descriptor_count() == before);
}

TEST(UnixDuplex, ConnectionFailureAndImmediateCancel) {
    auto absent =
        Endpoint::unix_socket(rstd::path::PathBuf::from("/missing/ncrequest-duplex"_str)).unwrap();
    auto wire = UnixDuplex::make(rstd::move(absent)).unwrap();
    EXPECT_TRUE(block_on(wire->connected()).unwrap().is_err());
    for (unsigned i = 0; i < 10; ++i) {
        auto cancelled = UnixDuplex::make(endpoint()).unwrap();
        auto pending   = cancelled->connected();
        cancelled->cancel();
        auto result = block_on(rstd::move(pending)).unwrap();
        if (result.is_err()) EXPECT_TRUE(result.unwrap_err().is_Canceled());
        EXPECT_TRUE(block_on(cancelled->read()).unwrap().is_err());
    }
}

TEST(UnixDuplex, ErrorClonePreservesDetails) {
    auto original = Error::Client(
        ClientError { ClientBackend::Curl, i32(7), String::make("connect failed"_str) });
    auto cloned = original.clone();
    EXPECT_TRUE(cloned.as_Client().error.code == i32(7));
    EXPECT_TRUE(cloned.as_Client().error.message.as_str() == "connect failed"_str);
    auto io = Error::Io(rstd::io::error::Error::from_raw_os_error(i32(13)));
    EXPECT_TRUE(io.clone().as_Io().error.raw_os_error() == io.as_Io().error.raw_os_error());
    EXPECT_TRUE(Error::Canceled().clone().is_Canceled());
    EXPECT_TRUE(Error::Timeout().clone().is_Timeout());
}

TEST(UnixDuplex, BackpressureExpiresPendingWrite) {
    DuplexOptions options;
    options.io_timeout = rstd::time::Duration::from_millis(u64(80));
    auto wire          = UnixDuplex::make(endpoint(), options).unwrap();
    ASSERT_TRUE(block_on(wire->connected()).unwrap().is_ok());
    Vec<u8> payload;
    payload.resize(usize(65536), u8('B'));
    bool timed_out = false;
    for (unsigned i = 0; i < 64; ++i) {
        auto result = block_on(wire->write(Bytes::copy_from_slice(payload.as_slice()))).unwrap();
        if (result.is_err()) {
            EXPECT_TRUE(result.unwrap_err().is_Timeout());
            timed_out = true;
            break;
        }
        EXPECT_TRUE(*result == usize(65536));
    }
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(block_on(wire->read()).unwrap().is_err());
}

TEST(UnixDuplex, BoundsAndInvalidEndpoint) {
    EXPECT_TRUE(UnixDuplex::make(Endpoint::network()).is_err());
    DuplexOptions invalid;
    invalid.io_timeout = {};
    EXPECT_TRUE(UnixDuplex::make(endpoint(), invalid).is_err());
    auto wire = UnixDuplex::make(endpoint()).unwrap();
    ASSERT_TRUE(block_on(wire->connected()).unwrap().is_ok());
    Vec<u8> large;
    large.resize(usize(65537), u8());
    EXPECT_TRUE(block_on(wire->write(Bytes::copy_from_slice(large.as_slice()))).unwrap().is_err());
    EXPECT_TRUE(block_on(wire->write({})).unwrap().unwrap() == usize());
    wire->cancel();
}
