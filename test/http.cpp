#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
import ncrequest;
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
import ncrequest.curl;
#endif
import rstd;

namespace
{

using ncrequest::byte;
using ncrequest::usize;

struct FetchResult {
    bool        got_response { false };
    bool        got_body { false };
    int         code { 0 };
    bool        has_test_header { false };
    bool        finished_while_paused { false };
    usize       upload_callback_count { 0 };
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

struct ShareResult {
    FetchResult default_set;
    FetchResult share_set;
    FetchResult isolated_set;
    FetchResult default_echo;
    FetchResult share_echo;
    FetchResult isolated_echo;
    FetchResult cloned_echo;
    FetchResult redirect_echo;
    FetchResult temporary_request;
    ErrorResult canceled_request;
    ErrorResult timed_out_request;
    FetchResult recovered_echo;
    FetchResult fixture_echo;
    FetchResult persisted_echo;
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

auto download_body() -> std::string {
    std::string out;
    out.resize(256 * 1024);
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>((i * 37 + 11) % 256);
    }
    return out;
}

[[maybe_unused]] auto slow_stream_body() -> std::string {
    std::string out;
    out.reserve(17 * 8192 + 5);
    for (int i = 0; i < 8192; ++i) {
        out += "slow-stream-body-";
    }
    out += "done\n";
    return out;
}

auto upload_body() -> std::string {
    std::string out;
    out.reserve(192 * 1024);
    for (usize i = 0; i < 192 * 1024; ++i) {
        out.push_back(static_cast<char>((i * 19 + 5) % 256));
    }
    return out;
}

auto bytes_from_string(const std::string& body) -> rstd::bytes::Bytes {
    return rstd::bytes::Bytes::copy_from_slice(rstd::slice<rstd::u8>::from_raw_parts(
        reinterpret_cast<const rstd::u8*>(body.data()), body.size()));
}

auto string_from_bytes(const rstd::bytes::Bytes& bytes) -> std::string {
    if (bytes.size() == 0) return {};
    return std::string { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

auto unique_temp_path(std::string_view name) -> std::filesystem::path {
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("ncrequest-") + std::string(name) + "-" + std::to_string(ticks));
}

auto write_file(const std::filesystem::path& path, const std::string& body) -> bool {
    std::ofstream out { path, std::ios::binary };
    if (! out) return false;
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(out);
}

auto read_file(const std::filesystem::path& path) -> std::optional<std::string> {
    std::ifstream in { path, std::ios::binary };
    if (! in) return std::nullopt;
    return std::string { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void remove_file(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

auto response_code(ncrequest::Arc<ncrequest::Response> rsp) -> int {
    auto code = rsp->code();
    if (code.is_some()) return code.unwrap();
    return 0;
}

void record_error(ErrorResult& result, const ncrequest::Error& error) {
    result.got_error = true;
    result.kind      = error.kind();
    if (error.is_Client()) {
        result.backend     = error.as_Client().error.backend;
        result.client_code = error.as_Client().error.code;
    }
}

auto fetch_text_request(ncrequest::Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
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

auto fetch_text(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    return fetch_text_request(rstd::move(session), ncrequest::Request { url });
}

auto request_with_share(std::string url, const ncrequest::SessionShare& share)
    -> ncrequest::Request {
    auto request = ncrequest::Request { url };
    request.get_opt<ncrequest::req_opt::Share>().set_share(rstd::Some(share.clone()));
    return request;
}

auto cancel_request(ncrequest::Arc<ncrequest::Session>, ncrequest::Request)
    -> ncrequest::coro<ErrorResult>;
auto timeout_request(ncrequest::Arc<ncrequest::Session>, ncrequest::Request)
    -> ncrequest::coro<ErrorResult>;

auto fetch_after_request_drop(ncrequest::Arc<ncrequest::Session> session, std::string url,
                              const ncrequest::SessionShare& share)
    -> ncrequest::coro<FetchResult> {
    auto result   = FetchResult {};
    auto response = ncrequest::Arc<ncrequest::Response> {};
    {
        auto request = request_with_share(rstd::move(url), share);
        auto started = co_await session->get(request);
        if (started.is_err()) {
            result.error = "session returned error";
            co_return result;
        }
        response            = rstd::move(started).unwrap();
        result.got_response = true;
    }

    auto text = co_await response->text();
    if (text.is_err()) {
        result.error = "response text read failed";
        co_return result;
    }
    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = rstd::move(text).unwrap();
    result.got_body        = true;
    co_return result;
}

auto exercise_share(ncrequest::Arc<ncrequest::Session> session, std::string base,
                    std::filesystem::path cookie_file,
                    std::filesystem::path fixture_file) -> ncrequest::coro<ShareResult> {
    auto result   = ShareResult {};
    auto shared   = ncrequest::SessionShare {};
    auto isolated = ncrequest::SessionShare {};

    result.default_set = co_await fetch_text(
        session, local_http_url(base, "/cookie/set?name=default_cookie&value=default"));
    auto shared_task = rstd::async::spawn_local(fetch_text_request(
        session,
        request_with_share(
            local_http_url(base, "/cookie/set?name=shared_cookie&value=shared"), shared)));
    auto isolated_task = rstd::async::spawn_local(fetch_text_request(
        session,
        request_with_share(
            local_http_url(base, "/cookie/set?name=isolated_cookie&value=isolated"), isolated)));
    auto share_sets =
        co_await rstd::async::join(rstd::move(shared_task), rstd::move(isolated_task));
    result.share_set    = rstd::move(share_sets.get<0>()).unwrap();
    result.isolated_set = rstd::move(share_sets.get<1>()).unwrap();
    result.default_echo =
        co_await fetch_text(session, local_http_url(base, "/cookie/echo"));
    result.share_echo = co_await fetch_text_request(
        session, request_with_share(local_http_url(base, "/cookie/echo"), shared));
    result.isolated_echo = co_await fetch_text_request(
        session, request_with_share(local_http_url(base, "/cookie/echo"), isolated));

    auto second_session = ncrequest::Session::make();
    auto cloned         = shared.clone();
    result.cloned_echo  = co_await fetch_text_request(
        second_session, request_with_share(local_http_url(base, "/cookie/echo"), cloned));
    result.redirect_echo = co_await fetch_text_request(
        second_session,
        request_with_share(local_http_url(
                               base,
                               "/cookie/redirect-set?name=redirect_cookie&value=redirected"),
                           cloned));
    result.temporary_request = co_await fetch_after_request_drop(
        second_session,
        local_http_url(base, "/cookie/slow-set?name=lifetime_cookie&value=alive"),
        cloned);
    result.canceled_request = co_await cancel_request(
        second_session, request_with_share(local_http_url(base, "/slow-stream"), cloned));
    result.timed_out_request = co_await timeout_request(
        second_session, request_with_share(local_http_url(base, "/slow-first-byte"), cloned));
    result.recovered_echo = co_await fetch_text_request(
        second_session, request_with_share(local_http_url(base, "/cookie/echo"), cloned));

    auto fixture = ncrequest::SessionShare {};
    fixture.load(fixture_file);
    result.fixture_echo = co_await fetch_text_request(
        second_session, request_with_share(local_http_url(base, "/cookie/echo"), fixture));

    cloned.save(cookie_file);
    auto persisted = ncrequest::SessionShare {};
    persisted.load(cookie_file);
    result.persisted_echo = co_await fetch_text_request(
        second_session, request_with_share(local_http_url(base, "/cookie/echo"), persisted));
    co_return result;
}

auto fetch_bytes(ncrequest::Arc<ncrequest::Session> session, std::string url)
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

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = string_from_bytes(rstd::move(bytes).unwrap());
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

auto post_bytes(ncrequest::Arc<ncrequest::Session> session, std::string url, std::string body)
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

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = string_from_bytes(rstd::move(bytes).unwrap());
    result.got_body        = true;
    co_return result;
}

auto timeout_request(ncrequest::Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;
    auto&       timeout = req.get_opt<ncrequest::req_opt::Timeout>();
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
    timeout.transfer_timeout = 100;
#else
    timeout.low_speed        = 1;
    timeout.transfer_timeout = 1;
#endif

    auto rsp = co_await session->get(req);
    if (rsp.is_err()) {
        auto error = rstd::move(rsp).unwrap_err();
        record_error(result, error);
        co_return result;
    }
    result.got_response = true;

    auto text = co_await rsp.unwrap()->text();
    if (text.is_err()) {
        auto error = rstd::move(text).unwrap_err();
        record_error(result, error);
        co_return result;
    }

    result.error = "timeout request completed";
    co_return result;
}

auto fetch_timeout(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    return timeout_request(rstd::move(session), ncrequest::Request { url });
}

auto cancel_request(ncrequest::Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;

    auto rsp = co_await session->get(req);
    if (rsp.is_err()) {
        result.error = "session returned error before cancel";
        co_return result;
    }
    result.got_response = true;

    auto response = rsp.unwrap();
    response->cancel();

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        auto error = rstd::move(bytes).unwrap_err();
        record_error(result, error);
        co_return result;
    }

    result.error = "cancel request completed";
    co_return result;
}

auto fetch_then_cancel(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    return cancel_request(rstd::move(session), ncrequest::Request { url });
}

#ifdef NCREQUEST_CLIENT_BACKEND_CURL
auto curl_pause_recv(ncrequest::Arc<ncrequest::Session> session, std::string url)
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
    response->pause_recv(true);
    co_await rstd::async::sleep(rstd::time::Duration::from_millis(350));
    result.finished_while_paused = response->is_finished();
    response->pause_recv(false);

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed after pause";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.body            = string_from_bytes(rstd::move(bytes).unwrap());
    result.got_body        = true;
    co_return result;
}

auto curl_streaming_upload(ncrequest::Arc<ncrequest::Session> session, std::string url,
                           std::string body) -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req    = ncrequest::Request { url };
    usize       offset = 0;
    usize       calls  = 0;
    auto&       reader = req.get_opt<ncrequest::req_opt::Read>();
    reader.size        = body.size();
    reader.callback    = [&body, &offset, &calls](byte* ptr, usize size) -> usize {
        ++calls;
        auto remaining = body.size() - offset;
        auto copied    = remaining;
        if (copied > size) copied = size;
        if (copied > 4096) copied = 4096;
        if (copied == 0) return 0;
        std::memcpy(ptr, body.data() + offset, copied);
        offset += copied;
        return copied;
    };

    auto rsp = co_await session->post(req);
    if (rsp.is_err()) {
        result.error = "session returned error";
        co_return result;
    }

    auto response       = rsp.unwrap();
    result.got_response = true;

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed";
        co_return result;
    }

    result.code                  = response_code(response);
    result.has_test_header       = response->header().has_field("x-ncrequest-test");
    result.body                  = string_from_bytes(rstd::move(bytes).unwrap());
    result.upload_callback_count = calls;
    result.got_body              = true;
    co_return result;
}
#endif

template<typename Start>
auto run_http(Start&& start) {
    auto session = ncrequest::Session::make();
    return rstd::async::block_on(start(session));
}

auto rstd_wait_yield() -> ncrequest::coro<int> {
    co_await rstd::async::yield_now();
    co_return 42;
}

} // namespace

TEST(http, UrlEncoding) {
    EXPECT_EQ(ncrequest::url_encode("a b/+~"), "a%20b%2F%2B~");
    EXPECT_EQ(ncrequest::url_decode("a%20b%2Fb%ZZ+"), "a b/b%ZZ+");
}

TEST(http, RstdAsyncPollFuture) {
    auto value = rstd::async::block_on(rstd_wait_yield());
    EXPECT_EQ(value, 42);
}

TEST(http, ErrorModelVariants) {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    ncrequest::Error curl_error = rstd::into(curl::CURLcode::CURLE_COULDNT_CONNECT);
    EXPECT_EQ(curl_error.kind(), ncrequest::ErrorKind::Client);
    ASSERT_TRUE(curl_error.is_Client());
    EXPECT_EQ(curl_error.as_Client().error.backend, ncrequest::ClientBackend::Curl);
    EXPECT_EQ(curl_error.as_Client().error.code,
              static_cast<rstd::i32>(curl::CURLcode::CURLE_COULDNT_CONNECT));
#else
    auto client = ncrequest::Error::Client(ncrequest::ClientError {
        .backend = ncrequest::ClientBackend::QtNetwork,
        .code    = 7,
        .message = "client error",
    });
    EXPECT_EQ(client.kind(), ncrequest::ErrorKind::Client);
    ASSERT_TRUE(client.is_Client());
    EXPECT_EQ(client.as_Client().error.backend, ncrequest::ClientBackend::QtNetwork);
    EXPECT_EQ(client.as_Client().error.code, 7);
#endif

    auto io = rstd::io::error::Error::from_kind(
        rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::TimedOut });
    ncrequest::Error io_error = rstd::into(rstd::move(io));
    EXPECT_EQ(io_error.kind(), ncrequest::ErrorKind::Io);
    ASSERT_TRUE(io_error.is_Io());
    EXPECT_EQ(io_error.as_Io().error.kind(),
              (rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::TimedOut }));

    auto canceled = ncrequest::Error::Canceled();
    EXPECT_EQ(canceled.kind(), ncrequest::ErrorKind::Canceled);
}

TEST(http, RequestOptionEnumSetOpt) {
    auto req = ncrequest::Request {};

    req.set_opt(ncrequest::RequestOpt::Timeout(ncrequest::req_opt::Timeout {
        .low_speed        = 2,
        .connect_timeout  = 3,
        .transfer_timeout = 4,
    }));
    EXPECT_EQ(req.get_opt<ncrequest::req_opt::Timeout>().low_speed, 2);
    EXPECT_EQ(req.get_opt<ncrequest::req_opt::Timeout>().connect_timeout, 3);
    EXPECT_EQ(req.get_opt<ncrequest::req_opt::Timeout>().transfer_timeout, 4);

    req.set_opt(ncrequest::RequestOpt::Proxy(ncrequest::req_opt::Proxy {
        .type    = ncrequest::req_opt::Proxy::Type::SOCKS5,
        .content = "127.0.0.1:1080",
    }));
    EXPECT_EQ(req.get_opt<ncrequest::req_opt::Proxy>().type,
              ncrequest::req_opt::Proxy::Type::SOCKS5);
    EXPECT_EQ(req.get_opt<ncrequest::req_opt::Proxy>().content, "127.0.0.1:1080");

    req.set_opt(ncrequest::RequestOpt::SSL(ncrequest::req_opt::SSL {
        .verify_certificate = false,
    }));
    EXPECT_FALSE(req.get_opt<ncrequest::req_opt::SSL>().verify_certificate);

    auto share_opt = ncrequest::req_opt::Share {};
    share_opt.set_share(rstd::Some(ncrequest::SessionShare {}));
    req.set_opt(ncrequest::RequestOpt::Share(rstd::move(share_opt)));
    EXPECT_TRUE(req.get_opt<ncrequest::req_opt::Share>().share.is_some());
}

TEST(http, LocalHttpShareIsolationRedirectAndPersistence) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto cookie_file  = unique_temp_path("share-cookies.txt");
    auto fixture_file = unique_temp_path("share-fixture.txt");
    ASSERT_TRUE(write_file(fixture_file,
                           "# Netscape HTTP Cookie File\n"
                           "127.0.0.1\tFALSE\t/\tFALSE\t2147483647\tfixture_cookie\tfixture\n"
                           "#HttpOnly_127.0.0.1\tFALSE\t/\tFALSE\t2147483647\t"
                           "http_only_cookie\thttp-only\n"
                           "127.0.0.1\tFALSE\t/\tFALSE\t1\texpired_cookie\texpired\n"
                           "malformed\n"));
    auto result = run_http([base, cookie_file, fixture_file](auto session) {
        return exercise_share(session, base, cookie_file, fixture_file);
    });
    auto persisted = read_file(cookie_file);
    remove_file(cookie_file);
    remove_file(fixture_file);

    auto assert_fetch = [](const FetchResult& fetch) {
        ASSERT_TRUE(fetch.got_response) << fetch.error;
        ASSERT_TRUE(fetch.got_body) << fetch.error;
        EXPECT_EQ(fetch.code, 200);
    };
    assert_fetch(result.default_set);
    assert_fetch(result.share_set);
    assert_fetch(result.isolated_set);
    assert_fetch(result.default_echo);
    assert_fetch(result.share_echo);
    assert_fetch(result.isolated_echo);
    assert_fetch(result.cloned_echo);
    assert_fetch(result.temporary_request);
    assert_fetch(result.recovered_echo);
    assert_fetch(result.fixture_echo);
    assert_fetch(result.persisted_echo);
    ASSERT_TRUE(result.redirect_echo.got_response) << result.redirect_echo.error;
    ASSERT_TRUE(result.redirect_echo.got_body) << result.redirect_echo.error;
    ASSERT_TRUE(result.canceled_request.got_response) << result.canceled_request.error;
    ASSERT_TRUE(result.canceled_request.got_error) << result.canceled_request.error;
    EXPECT_EQ(result.canceled_request.kind, ncrequest::ErrorKind::Canceled);
    ASSERT_TRUE(result.timed_out_request.got_error) << result.timed_out_request.error;
    EXPECT_EQ(result.timed_out_request.kind, ncrequest::ErrorKind::Client);

    ASSERT_TRUE(persisted.has_value());
    EXPECT_FALSE(persisted->empty());
    EXPECT_NE(result.default_echo.body.find("default_cookie=default"), std::string::npos);
    EXPECT_EQ(result.default_echo.body.find("shared_cookie=shared"), std::string::npos);
    EXPECT_NE(result.share_echo.body.find("shared_cookie=shared"), std::string::npos);
    EXPECT_EQ(result.share_echo.body.find("default_cookie=default"), std::string::npos);
    EXPECT_NE(result.isolated_echo.body.find("isolated_cookie=isolated"), std::string::npos);
    EXPECT_EQ(result.isolated_echo.body.find("shared_cookie=shared"), std::string::npos);
    EXPECT_EQ(result.isolated_echo.body.find("default_cookie=default"), std::string::npos);
    EXPECT_NE(result.cloned_echo.body.find("shared_cookie=shared"), std::string::npos);
    EXPECT_EQ(result.cloned_echo.body.find("default_cookie=default"), std::string::npos);
    EXPECT_NE(result.redirect_echo.body.find("shared_cookie=shared"), std::string::npos);
    EXPECT_NE(result.redirect_echo.body.find("redirect_cookie=redirected"), std::string::npos);
    EXPECT_EQ(result.redirect_echo.body.find("default_cookie=default"), std::string::npos);
    EXPECT_EQ(result.temporary_request.body, "cookie set slowly\n");
    EXPECT_NE(result.recovered_echo.body.find("lifetime_cookie=alive"), std::string::npos);
    EXPECT_NE(result.recovered_echo.body.find("redirect_cookie=redirected"),
              std::string::npos);
    EXPECT_EQ(result.recovered_echo.body.find("default_cookie=default"), std::string::npos);
    EXPECT_NE(result.fixture_echo.body.find("fixture_cookie=fixture"), std::string::npos);
    EXPECT_NE(result.fixture_echo.body.find("http_only_cookie=http-only"), std::string::npos);
    EXPECT_EQ(result.fixture_echo.body.find("expired_cookie=expired"), std::string::npos);
    EXPECT_NE(result.persisted_echo.body.find("shared_cookie=shared"), std::string::npos);
    EXPECT_NE(result.persisted_echo.body.find("redirect_cookie=redirected"), std::string::npos);
    EXPECT_NE(result.persisted_echo.body.find("lifetime_cookie=alive"), std::string::npos);
    EXPECT_EQ(result.persisted_echo.body.find("default_cookie=default"), std::string::npos);
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

TEST(http, LocalHttpDownloadFile) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/download.bin")](auto session) {
        return fetch_bytes(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, download_body());

    auto path = unique_temp_path("download.bin");
    ASSERT_TRUE(write_file(path, result.body));
    auto stored = read_file(path);
    remove_file(path);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, download_body());
}

TEST(http, LocalHttpUploadFile) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto payload = upload_body();
    auto path    = unique_temp_path("upload.bin");
    ASSERT_TRUE(write_file(path, payload));
    auto stored = read_file(path);
    remove_file(path);
    ASSERT_TRUE(stored.has_value());

    auto result = run_http([url = local_http_url(base, "/upload"), body = *stored](auto session) {
        return post_bytes(session, url, body);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, payload);
}

TEST(http, LocalHttpTimeout) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/slow-first-byte")](auto session) {
        return fetch_timeout(session, url);
    });
    ASSERT_TRUE(result.got_error) << result.error;
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Client);
    EXPECT_EQ(result.backend, ncrequest::ClientBackend::QtNetwork);
#else
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Client);
    EXPECT_EQ(result.backend, ncrequest::ClientBackend::Curl);
    EXPECT_EQ(result.client_code,
              static_cast<rstd::i32>(curl::CURLcode::CURLE_OPERATION_TIMEDOUT));
#endif
}

TEST(http, LocalHttpCancel) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/slow-stream")](auto session) {
        return fetch_then_cancel(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_error) << result.error;
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Canceled);
}

TEST(http, LocalHttpCurlPauseRecv) {
#ifndef NCREQUEST_CLIENT_BACKEND_CURL
    GTEST_SKIP() << "curl-only recv pause test";
#else
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/slow-stream")](auto session) {
        return curl_pause_recv(session, url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_FALSE(result.finished_while_paused);
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, slow_stream_body());
#endif
}

TEST(http, LocalHttpCurlStreamingUpload) {
#ifndef NCREQUEST_CLIENT_BACKEND_CURL
    GTEST_SKIP() << "curl-only streaming upload test";
#else
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto payload = upload_body();
    auto result  = run_http([url = local_http_url(base, "/upload"), payload](auto session) {
        return curl_streaming_upload(session, url, payload);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, payload);
    EXPECT_GT(result.upload_callback_count, 1u);
#endif
}
