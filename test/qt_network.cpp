#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

import ncrequest.qt_network;
import rstd;

namespace
{

struct FetchResult {
    bool        got_response { false };
    bool        got_body { false };
    int         code { 0 };
    bool        has_test_header { false };
    std::string body;
    std::string error;
};

struct ErrorResult {
    bool                     got_response { false };
    bool                     got_error { false };
    ncrequest::ErrorKind     kind { ncrequest::ErrorKind::InvalidState };
    ncrequest::ClientBackend backend { ncrequest::ClientBackend::QtNetwork };
    int                      client_code { 0 };
    std::string              error;
};

auto local_http_base_url() -> std::string {
    auto* value = std::getenv("NCREQUEST_TEST_HTTP_BASE_URL");
    if (value == nullptr || *value == '\0') return {};
    return value;
}

auto local_http_url(std::string_view base, std::string_view path) -> std::string {
    std::string out { base };
    if (! out.empty() && out.back() == '/' && ! path.empty() && path.front() == '/') {
        out.pop_back();
    } else if (! out.empty() && out.back() != '/' && ! path.empty() && path.front() != '/') {
        out.push_back('/');
    }
    out.append(path);
    return out;
}

auto large_body() -> std::string {
    std::string out;
    out.reserve(16 * 8192 + 5);
    for (int i = 0; i < 8192; ++i) {
        out += "0123456789abcdef";
    }
    out += "tail\n";
    return out;
}

auto bytes_from_string(const std::string& body) -> rstd::bytes::Bytes {
    return rstd::bytes::Bytes::copy_from_slice(rstd::slice<rstd::u8>::from_raw_parts(
        reinterpret_cast<const rstd::u8*>(body.data()), body.size()));
}

auto response_code(ncrequest::Arc<ncrequest::qt_network::Response> response) -> int {
    auto code = response->code();
    if (code.is_some()) return code.unwrap();
    return 0;
}

auto fetch_text(ncrequest::Arc<ncrequest::qt_network::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = ncrequest::Request { url };
    auto        rsp = co_await session->get(req);
    if (rsp.is_err()) {
        result.error = "session request failed";
        co_return result;
    }

    auto response       = rsp.unwrap();
    result.got_response = true;

    auto text = co_await response->text();
    if (text.is_err()) {
        result.error = "response text read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = text.unwrap();
    result.got_body        = true;
    co_return result;
}

auto post_text(ncrequest::Arc<ncrequest::qt_network::Session> session, std::string url,
               std::string body) -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = ncrequest::Request { url };
    auto        rsp = co_await session->post(req, bytes_from_string(body));
    if (rsp.is_err()) {
        result.error = "session request failed";
        co_return result;
    }

    auto response       = rsp.unwrap();
    result.got_response = true;

    auto text = co_await response->text();
    if (text.is_err()) {
        result.error = "response text read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = text.unwrap();
    result.got_body        = true;
    co_return result;
}

auto fetch_timeout(ncrequest::Arc<ncrequest::qt_network::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;
    auto        req                                             = ncrequest::Request { url };
    req.get_opt<ncrequest::req_opt::Timeout>().transfer_timeout = 100;

    auto rsp = co_await session->get(req);
    if (rsp.is_err()) {
        result.error = "session request failed";
        co_return result;
    }
    result.got_response = true;

    auto text = co_await rsp.unwrap()->text();
    if (text.is_err()) {
        auto error       = rstd::move(text).unwrap_err();
        result.got_error = true;
        result.kind      = error.kind();
        if (error.is_Client()) {
            result.backend     = error.as_Client().error.backend;
            result.client_code = error.as_Client().error.code;
        }
    }
    co_return result;
}

auto fetch_with_share(ncrequest::Arc<ncrequest::qt_network::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    auto result = ErrorResult {};
    auto req    = ncrequest::Request { url };
    req.get_opt<ncrequest::req_opt::Share>().set_share(
        rstd::Some(ncrequest::SessionShare {}));

    auto response = co_await session->get(req);
    if (response.is_err()) {
        auto error       = rstd::move(response).unwrap_err();
        result.got_error = true;
        result.kind      = error.kind();
        co_return result;
    }
    result.got_response = true;
    co_return result;
}

auto share_roundtrip(ncrequest::Arc<ncrequest::qt_network::Session> session, std::string base)
    -> ncrequest::coro<FetchResult> {
    auto share = ncrequest::SessionShare {};
    auto set_request = ncrequest::Request {
        local_http_url(base, "/cookie/set?name=owned_manager_cookie&value=shared")
    };
    set_request.get_opt<ncrequest::req_opt::Share>().set_share(rstd::Some(share.clone()));
    auto set_response = co_await session->get(set_request);
    if (set_response.is_err()) {
        auto result  = FetchResult {};
        result.error = "share cookie set failed";
        co_return result;
    }
    auto set_body = co_await rstd::move(set_response).unwrap()->text();
    if (set_body.is_err()) {
        auto result  = FetchResult {};
        result.error = "share cookie set body failed";
        co_return result;
    }

    auto echo_request = ncrequest::Request { local_http_url(base, "/cookie/echo") };
    echo_request.get_opt<ncrequest::req_opt::Share>().set_share(rstd::Some(share.clone()));
    auto echo_response = co_await session->get(echo_request);
    if (echo_response.is_err()) {
        auto result  = FetchResult {};
        result.error = "share cookie echo failed";
        co_return result;
    }

    auto result          = FetchResult {};
    auto response        = rstd::move(echo_response).unwrap();
    result.got_response  = true;
    auto echo_body       = co_await response->text();
    if (echo_body.is_err()) {
        result.error = "share cookie echo body failed";
        co_return result;
    }
    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = rstd::move(echo_body).unwrap();
    result.got_body        = true;
    co_return result;
}

auto fetch_then_cancel(ncrequest::Arc<ncrequest::qt_network::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;
    auto        req = ncrequest::Request { url };

    auto rsp = co_await session->get(req);
    if (rsp.is_err()) {
        result.error = "session request failed";
        co_return result;
    }
    result.got_response = true;

    auto response = rsp.unwrap();
    response->cancel();

    auto text = co_await response->text();
    if (text.is_err()) {
        auto error       = rstd::move(text).unwrap_err();
        result.got_error = true;
        result.kind      = error.kind();
    }
    co_return result;
}

template<typename Start>
auto run_http(Start&& start) {
    auto session = ncrequest::qt_network::Session::make();
    return rstd::async::block_on(start(session));
}

template<typename Start>
auto run_http_rstd(Start&& start) {
    auto session = ncrequest::qt_network::Session::make();
    return rstd::async::block_on(start(session));
}

template<typename Start>
auto run_http_rstd_multi_thread(Start&& start) {
    auto runtime_result = rstd::async::RuntimeBuilder::multi_thread().worker_threads(2).build();
    auto runtime        = runtime_result.unwrap();
    auto session        = ncrequest::qt_network::Session::make();
    return runtime.block_on(start(session));
}

template<typename T>
auto run_qt_owner_coro(ncrequest::coro<T> task) -> T {
    QEventLoop       loop;
    std::optional<T> result;
    auto             worker = std::thread([&loop, &result, task = rstd::move(task)]() mutable {
        result.emplace(rstd::async::block_on(rstd::move(task)));
        (void)QMetaObject::invokeMethod(
            &loop,
            [&loop] {
                loop.quit();
            },
            Qt::QueuedConnection);
    });

    loop.exec();
    worker.join();
    return rstd::move(*result);
}

} // namespace

TEST(qt_network, LocalHttpGetText) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/text")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(qt_network, LocalHttpGetTextRstdBlockOn) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http_rstd([url = local_http_url(base, "/text")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(qt_network, LocalHttpGetTextRstdMultiThreadRuntime) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http_rstd_multi_thread([url = local_http_url(base, "/text")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(qt_network, LocalHttpLargeBody) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/large")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, large_body());
}

TEST(qt_network, LocalHttpNotFoundBody) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/missing")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 404);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "missing\n");
}

TEST(qt_network, LocalHttpServerErrorBody) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/server-error")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 500);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "server error\n");
}

TEST(qt_network, LocalHttpPostEcho) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto payload = std::string { "ncrequest qt post payload\nwith a second line\n" };
    auto result  = run_http([url = local_http_url(base, "/echo"), payload](auto session) {
        return post_text(session, url, payload);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, payload);
}

TEST(qt_network, LocalHttpTimeout) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/delay")](auto session) {
        return fetch_timeout(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_error) << result.error;
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Client);
    EXPECT_EQ(result.backend, ncrequest::ClientBackend::QtNetwork);
}

TEST(qt_network, LocalHttpCancel) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/delay")](auto session) {
        return fetch_then_cancel(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_error) << result.error;
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Canceled);
}

TEST(qt_network, LocalHttpManagerAutoDeleteOverride) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    QNetworkAccessManager manager;
    manager.setAutoDeleteReplies(true);
    auto session = ncrequest::qt_network::Session::make(&manager);

    auto result = run_qt_owner_coro(fetch_text(session, local_http_url(base, "/text")));
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(qt_network, LocalHttpExternalManagerRejectsShare) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    QNetworkAccessManager manager;
    auto session = ncrequest::qt_network::Session::make(&manager);

    auto result =
        run_qt_owner_coro(fetch_with_share(session, local_http_url(base, "/cookie/echo")));
    EXPECT_FALSE(result.got_response);
    ASSERT_TRUE(result.got_error) << result.error;
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::InvalidState);
}

TEST(qt_network, LocalHttpOwnedManagerSupportsShare) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    QObject parent;
    auto session = ncrequest::qt_network::Session::make(&parent);

    auto result = run_qt_owner_coro(share_roundtrip(session, base));
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_NE(result.body.find("owned_manager_cookie=shared"), std::string::npos);
}
