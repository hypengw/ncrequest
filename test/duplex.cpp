#if ! defined(LITO_FEAT_QT) && defined(__linux__)
#    include <rstd/test/gtest.hpp>
#    include <cstdlib>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
using rstd::async::block_on;
using rstd::bytes::Bytes;

static auto address(const char* variable = "NCREQUEST_DUPLEX_URL") -> lihttpto::Url {
    auto value = rstd::ffi::CStr::from_ptr(::getenv(variable)).to_str().unwrap();
    return lihttpto::Url::parse_http(value).unwrap();
}
TEST(NetworkDuplex, ConcurrentReadWriteAndHalfClose) {
    auto wire = Duplex::connect(address()).unwrap();
    ASSERT_TRUE(block_on(wire->connected()).unwrap().is_ok());
    auto pending = wire->read();
    EXPECT_TRUE(block_on(wire->read()).unwrap().is_err());
    ASSERT_TRUE(
        block_on(wire->write(Bytes::copy_from_slice("echo"_str.as_bytes()))).unwrap().is_ok());
    ASSERT_TRUE(block_on(wire->close_input()).unwrap().is_ok());
    String text;
    auto   first = block_on(rstd::move(pending)).unwrap().unwrap();
    for (auto byte : first.as_slice()) text.push_ascii(u8(byte));
    for (;;) {
        auto next = block_on(wire->read()).unwrap().unwrap();
        if (next.is_empty()) break;
        for (auto byte : next.as_slice()) text.push_ascii(u8(byte));
    }
    EXPECT_TRUE(text.as_str() == "echo:after-eof"_str);
}
TEST(NetworkDuplex, CancellationTimeoutAndBounds) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        DuplexOptions options;
        options.io_timeout = rstd::time::Duration::from_millis(u64(50));
        auto wire          = Duplex::connect(address(), options).unwrap();
        ASSERT_TRUE(block_on(wire->connected()).unwrap().is_ok());
        Vec<u8> large;
        large.resize(usize(65537), u8());
        EXPECT_TRUE(
            block_on(wire->write(Bytes::copy_from_slice(large.as_slice()))).unwrap().is_err());
        auto pending = wire->read();
        if (mode == 0) wire->cancel();
        if (mode == 2) {
            pending.close();
            EXPECT_TRUE(block_on(wire->write(Bytes::copy_from_slice("echo"_str.as_bytes())))
                            .unwrap()
                            .is_err());
        } else {
            auto result = block_on(rstd::move(pending)).unwrap();
            ASSERT_TRUE(result.is_err());
            EXPECT_TRUE(mode == 0 ? result.unwrap_err().is_Canceled()
                                  : result.unwrap_err().is_Timeout());
        }
    }
}
TEST(NetworkDuplex, UpgradePreservesAuthorizationAndPendingBytes) {
    lihttpto::Headers headers;
    headers.add("Authorization"_str, "Bearer local-test"_str).unwrap();
    auto request =
        lihttpto::UpgradeRequest::make(lihttpto::Method::parse("GET"_str).unwrap(),
                                       "/upgrade"_str,
                                       "localhost"_str,
                                       lihttpto::UpgradeProtocol::make("test"_str).unwrap(),
                                       rstd::move(headers))
            .unwrap();
    auto opened = block_on(UpgradeConnection::open(address(), rstd::move(request)));
    ASSERT_TRUE(opened.is_ok());
    auto   connection = rstd::move(opened).unwrap();
    String result;
    while (result.len() < usize(5)) {
        auto bytes = block_on(connection.read()).unwrap();
        ASSERT_TRUE(! bytes.is_empty());
        for (auto byte : bytes.as_slice()) result.push_ascii(u8(byte));
    }
    EXPECT_TRUE(result.as_str() == "ready"_str);
    connection.cancel();
}
TEST(NetworkDuplex, RejectsInvalidUrlAndDeadlines) {
    EXPECT_TRUE(Duplex::connect(lihttpto::Url::parse("file:///tmp/input"_str).unwrap()).is_err());
    EXPECT_TRUE(
        Duplex::connect(lihttpto::Url::parse("http://user@localhost/"_str).unwrap()).is_err());
    EXPECT_TRUE(
        Duplex::connect(lihttpto::Url::parse("http://localhost/#fragment"_str).unwrap()).is_err());
    DuplexOptions options;
    options.connect_timeout = {};
    EXPECT_TRUE(Duplex::connect(address(), options).is_err());
}
TEST(NetworkDuplexTls, RejectsUntrustedCertificateAndHalfClose) {
    auto wire   = Duplex::connect(address("NCREQUEST_TEST_HTTPS_URL")).unwrap();
    auto result = block_on(wire->connected()).unwrap();
    ASSERT_TRUE(result.is_err());
    ASSERT_TRUE(result.unwrap_err().is_Client());
    EXPECT_TRUE(result.unwrap_err().as_Client().error.code == i32(60));
    auto half_close = block_on(wire->close_input()).unwrap();
    EXPECT_TRUE(half_close.is_err() && half_close.unwrap_err().is_Unsupported());
}
#endif
