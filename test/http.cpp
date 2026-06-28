#include <future>
#include <gtest/gtest.h>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
#    include <QEventLoop>
#    include <QTimer>
#endif

import ncrequest;
import ncrequest.curl;
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

auto response_code(ncrequest::Arc<ncrequest::Response> rsp) -> int {
    auto code = rsp->code();
    if (code.is_some()) return code.unwrap();
    return 0;
}

auto fetch_text(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = ncrequest::Request { url };
    auto        rsp = co_await session->get(req);
    if (rsp.is_err()) {
        result.error = "session returned error";
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

auto post_text(ncrequest::Arc<ncrequest::Session> session, std::string url, std::string body)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = ncrequest::Request { url };
    auto        rsp = co_await session->post(req, bytes_from_string(body));
    if (rsp.is_err()) {
        result.error = "session returned error";
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

struct WakeTarget {
    virtual void schedule_poll() = 0;
    virtual ~WakeTarget()        = default;
};

#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
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
#endif

template<typename Start>
auto run_http(Start&& start) -> FetchResult {
    auto session = ncrequest::Session::make();
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
    return run_qt_future(start(session));
#else
    return rstd::async::block_on(start(session));
#endif
}

auto rstd_wait_yield() -> ncrequest::coro<int> {
    co_await rstd::async::yield_now();
    co_return 42;
}

} // namespace

TEST(http, RstdAsyncPollFuture) {
    auto value = rstd::async::block_on(rstd_wait_yield());
    EXPECT_EQ(value, 42);
}

TEST(http, ErrorModelVariants) {
    ncrequest::Error curl_error = rstd::into(curl::CURLcode::CURLE_COULDNT_CONNECT);
    EXPECT_EQ(curl_error.kind(), ncrequest::ErrorKind::Curl);
    ASSERT_TRUE(curl_error.is_Curl());
    EXPECT_EQ(curl_error.as_Curl().code, curl::CURLcode::CURLE_COULDNT_CONNECT);

    auto io = rstd::io::error::Error::from_kind(
        rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::TimedOut });
    ncrequest::Error io_error = rstd::into(rstd::move(io));
    EXPECT_EQ(io_error.kind(), ncrequest::ErrorKind::Io);
    ASSERT_TRUE(io_error.is_Io());
    EXPECT_EQ(io_error.as_Io().error.kind(),
              (rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::TimedOut }));

    auto canceled = ncrequest::Error::Canceled();
    EXPECT_EQ(canceled.kind(), ncrequest::ErrorKind::Canceled);

    auto client = ncrequest::Error::Client(ncrequest::ClientError {
        .backend = ncrequest::ClientBackend::QtNetwork,
        .code    = 7,
        .message = "client error",
    });
    EXPECT_EQ(client.kind(), ncrequest::ErrorKind::Client);
    ASSERT_TRUE(client.is_Client());
    EXPECT_EQ(client.as_Client().error.backend, ncrequest::ClientBackend::QtNetwork);
    EXPECT_EQ(client.as_Client().error.code, 7);
}

TEST(http, LocalHttpGetText) {
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

TEST(http, LocalHttpLargeBody) {
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

TEST(http, LocalHttpNoContent) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/empty")](auto session) {
        return fetch_text(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 204);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_TRUE(result.body.empty());
}

TEST(http, LocalHttpNotFoundBody) {
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

TEST(http, LocalHttpPostEcho) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto payload = std::string { "ncrequest post payload\nwith a second line\n" };
    auto result  = run_http([url = local_http_url(base, "/echo"), payload](auto session) {
        return post_text(session, url, payload);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, payload);
}
