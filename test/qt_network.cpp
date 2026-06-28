#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QTimer>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

struct WakeTarget {
    virtual void schedule_poll() = 0;
    virtual ~WakeTarget()        = default;
};

void  waker_drop(void*) {}
void* waker_clone(void* data) { return data; }
void  waker_wake(void* data) { static_cast<WakeTarget*>(data)->schedule_poll(); }
void  waker_wake_by_ref(void* data) { static_cast<WakeTarget*>(data)->schedule_poll(); }

const rstd::task::RawWakerVTable QT_WAKER_VTABLE {
    &waker_clone,
    &waker_wake,
    &waker_wake_by_ref,
    &waker_drop,
};

template<rstd::future::FutureLike F>
auto run_qt_future(F future) -> rstd::future::future_output_t<F> {
    using Output = rstd::future::future_output_t<F>;

    struct Runner : WakeTarget {
        F&                    future;
        QEventLoop            loop;
        std::optional<Output> result;
        bool                  polling { false };
        bool                  poll_posted { false };
        bool                  timed_out { false };
        rstd::task::Waker     waker;
        rstd::task::Context   cx;

        explicit Runner(F& future)
            : future(future),
              waker(rstd::task::Waker::from_raw(rstd::task::RawWaker { this, &QT_WAKER_VTABLE })),
              cx(waker) {}

        void schedule_poll() override {
            if (poll_posted || result.has_value()) return;
            poll_posted = true;
            QTimer::singleShot(0, &loop, [this] {
                poll_posted = false;
                poll_once();
            });
        }

        void poll_once() {
            if (polling || result.has_value()) return;
            polling  = true;
            auto out = rstd::future::poll(future, cx);
            if (out.is_ready()) {
                result.emplace(rstd::move(out).take());
                loop.quit();
            }
            polling = false;
        }
    };

    Runner runner { future };
    QTimer::singleShot(10000, &runner.loop, [&runner] {
        runner.timed_out = true;
        runner.loop.quit();
    });

    runner.poll_once();
    if (! runner.result.has_value()) {
        runner.loop.exec();
    }

    if (! runner.result.has_value() || runner.timed_out) {
        throw std::runtime_error("Qt future timed out");
    }

    return rstd::move(*runner.result);
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
    return run_qt_future(start(session));
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

    auto result = run_qt_future(fetch_text(session, local_http_url(base, "/text")));
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}
