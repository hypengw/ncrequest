#include <rstd/test/gtest.hpp>
#include <type_traits>
import ncrequest;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using ncrequest::EffectiveOptions;
using ncrequest::ErrorKind;
using ncrequest::Request;
using ncrequest::RequestOptions;
using ncrequest::Session;
using ncrequest::SessionOptions;
using rstd::async::block_on;
using rstd::path::PathBuf;
using rstd::time::Duration;

static_assert(! std::is_default_constructible_v<Session>);
static_assert(! std::is_constructible_v<Session, SessionOptions>);

TEST(session, FactoryValidatesDefaults) {
    auto options                = SessionOptions {};
    options.limits.header_bytes = usize();
    auto bad                    = Session::make(rstd::move(options));
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(bad.unwrap_err().kind(), ErrorKind::InvalidState);
    options               = SessionOptions {};
    options.timeout.total = ncrequest::TimeoutLimit::after(Duration {});
    EXPECT_TRUE(Session::make(rstd::move(options)).is_err());
    options               = SessionOptions {};
    options.tls.ca_bundle = Some(PathBuf {});
    EXPECT_TRUE(Session::make(rstd::move(options)).is_err());
    options          = SessionOptions {};
    options.endpoint = ncrequest::Endpoint::unix_socket(PathBuf::from("/tmp/socket"_str)).unwrap();
    options.tcp.keepalive = true;
    EXPECT_TRUE(Session::make(rstd::move(options)).is_err());
}

TEST(session, FactoryCloseIsIdempotent) {
    for (int i = 0; i < 8; ++i) {
        auto made = Session::make();
        ASSERT_TRUE(made.is_ok());
        auto session = rstd::move(made).unwrap();
        session->close();
        session->close();
        auto request = Request::from_url("http://127.0.0.1:1/"_str).unwrap();
        auto sent    = block_on(session->send(rstd::move(request)));
        ASSERT_TRUE(sent.is_err());
        EXPECT_TRUE(sent.unwrap_err().is_Canceled());
    }
}

TEST(session, EffectiveDefaultsCanBeOverridden) {
    auto options                  = SessionOptions {};
    options.limits.collect_bytes  = usize(8);
    auto defaults                 = EffectiveOptions::resolve(options, RequestOptions {}).unwrap();
    auto request                  = RequestOptions {};
    request.limits                = Some(ncrequest::ResourceLimits {});
    request.limits->collect_bytes = usize(16);
    auto overridden               = defaults.with_request(request).unwrap();
    EXPECT_EQ(overridden.limits().collect_bytes.to_primitive(), 16u);
    EXPECT_EQ(defaults.limits().collect_bytes.to_primitive(), 8u);
    request.limits->header_bytes = usize();
    EXPECT_TRUE(defaults.with_request(request).is_err());
}

TEST(session, BackendStartupErrors) {
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto options                  = ncrequest::CurlOptions {};
    options.max_total_connections = -1;
    auto failed                   = ncrequest::client::curl::SessionBackend::make(options);
    ASSERT_TRUE(failed.is_err());
    auto error = rstd::move(failed).unwrap_err();
    ASSERT_TRUE(error.is_Client());
    EXPECT_EQ(error.as_Client().error.backend, ncrequest::ClientBackend::CurlMulti);
    EXPECT_EQ(error.as_Client().error.code,
              static_cast<i32>(curl::CURLMcode::CURLM_BAD_FUNCTION_ARGUMENT));
    auto share = rstd::into<ncrequest::Error>(
        ncrequest::CurlMultiError::Share(curl::CURLSHcode::CURLSHE_BAD_OPTION));
    ASSERT_TRUE(share.is_Client());
    EXPECT_EQ(share.as_Client().error.backend, ncrequest::ClientBackend::CurlShare);
    EXPECT_EQ(share.as_Client().error.code, static_cast<i32>(curl::CURLSHcode::CURLSHE_BAD_OPTION));
    auto made = ncrequest::client::curl::SessionBackend::make();
    ASSERT_TRUE(made.is_ok());
    auto started = (*made)->start();
    EXPECT_TRUE(started.is_ok());
    (*made)->close();
#else
    auto failed = Session::from_qt_manager(nullptr);
    ASSERT_TRUE(failed.is_err());
    EXPECT_TRUE(failed.unwrap_err().is_InvalidState());
#endif
}

#ifdef NCREQUEST_CLIENT_BACKEND_CURL
TEST(session, CurlEasyKeepsConfigurationError) {
    auto session = Session::make().unwrap();
    auto easy    = ncrequest::CurlEasy {};
    ASSERT_EQ(easy.status(), curl::CURLcode::CURLE_OK);
    auto headers = lihttpto::Headers {};
    headers.add("X-Test"_str, "first"_str).unwrap();
    EXPECT_EQ(easy.set_header(headers), curl::CURLcode::CURLE_OK);
    EXPECT_EQ(easy.reset_header(), curl::CURLcode::CURLE_OK);
    auto code = easy.setopt(static_cast<curl::CURLoption>(99999), 0L);
    ASSERT_EQ(code, curl::CURLcode::CURLE_UNKNOWN_OPTION);
    EXPECT_EQ(easy.setopt(curl::CURLoption::CURLOPT_NOSIGNAL, 1L), code);
    EXPECT_EQ(easy.set_header(headers), code);
    EXPECT_EQ(easy.status(), code);
    EXPECT_EQ(easy.perform(), code);
    auto multi = ncrequest::CurlMulti {};
    auto added = multi.add_handle(easy);
    ASSERT_TRUE(added.is_err());
    EXPECT_EQ(added.unwrap_err().as_Easy().code, code);
}
#endif
