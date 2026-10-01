#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <rstd/enum.hpp>
#include <string>
#include <string_view>
import ncrequest;
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
import ncrequest.curl;
#endif
import rstd;
import rstd.cppstd;

using namespace rstd::literals;
using namespace rstd::prelude;
using IoError = rstd::io::error::Error;
using IoErrorKind = rstd::io::error::ErrorKind;
using ncrequest::byte;
using ncrequest::Endpoint;
using ncrequest::EffectiveOptions;
using ncrequest::PreparedRequest;
using ncrequest::RequestOptions;
using ncrequest::SessionOptions;
using Proxy = ncrequest::ProxyOptions;
using Share = ncrequest::ShareOptions;
using SSL = ncrequest::TlsOptions;
using Tcp = ncrequest::TcpOptions;
using Timeout = ncrequest::TimeoutOptions;
using ncrequest::BodyReader;
using ncrequest::RequestBody;
using ncrequest::usize;
using rstd::async::block_on;
using rstd::async::join;
using rstd::async::RuntimeBuilder;
using rstd::async::sleep;
using rstd::async::spawn_local;
using rstd::async::yield_now;
using rstd::bytes::Bytes;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::cppstd::to_string;
using rstd::env::temp_dir;
using rstd::fs::read;
using rstd::fs::write;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::time::Duration;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace
{

auto as_rstd_str(std::string_view value) -> ref<str> {
    return rstd::move(as_str(value)).unwrap();
}

struct FetchResult {
    bool                 got_response { false };
    bool                 got_body { false };
    bool                 got_error { false };
    int                  code { 0 };
    bool                 has_test_header { false };
    std::size_t          set_cookie_count { 0 };
    bool                 finished_while_paused { false };
    std::size_t          upload_callback_count { 0 };
    std::size_t          trailer_count { 0 };
    bool                 initial_has_trailer { false };
    std::string          body;
    std::string          first_set_cookie_name;
    std::string          repeated_header_values;
    std::string          error;
    ncrequest::ErrorKind error_kind { ncrequest::ErrorKind::InvalidState };
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

auto make_request(std::string_view url) -> ncrequest::Request {
    return rstd::move(ncrequest::Request::from_url(as_rstd_str(url))).unwrap();
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
    for (std::size_t i = 0; i < out.size(); ++i) {
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
    for (std::size_t i = 0; i < 192 * 1024; ++i) {
        out.push_back(static_cast<char>((i * 19 + 5) % 256));
    }
    return out;
}

auto bytes_from_string(const std::string& body) -> Bytes {
    auto bytes = slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(body.data()),
                                                   usize(body.size()));
    return Bytes::copy_from_slice(bytes);
}

auto string_from_bytes(const Bytes& bytes) -> std::string {
    if (bytes.size() == usize()) return {};
    return std::string { reinterpret_cast<const char*>(bytes.data()), bytes.size().to_primitive() };
}

auto unique_temp_path(std::string_view name) -> PathBuf {
    auto ticks    = steady_clock::now().time_since_epoch().count();
    auto path     = temp_dir();
    auto filename = std::string("ncrequest-") + std::string(name) + "-" + std::to_string(ticks);
    path.push(ref<Path>(as_rstd_str(filename)));
    return path;
}

auto write_file(ref<Path> path, const std::string& body) -> bool {
    auto bytes =
        slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(body.data()), usize(body.size()));
    auto owned = Vec<u8>::from(bytes);
    return write(path, owned.as_slice()).is_ok();
}

auto read_file(ref<Path> path) -> std::optional<std::string> {
    auto input = read(path);
    if (input.is_err()) return std::nullopt;
    auto bytes = rstd::move(input).unwrap();
    if (bytes.is_empty()) return std::string {};
    return std::string { reinterpret_cast<const char*>(bytes.data()), bytes.len().to_primitive() };
}

void remove_file(ref<Path> path) { (void)rstd::fs::remove_file(path); }

auto response_code(const ncrequest::Arc<ncrequest::Response>& rsp) -> int {
    auto code = rsp->code();
    if (code.is_some()) return code.unwrap().to_primitive();
    return 0;
}

void record_error(ErrorResult& result, const ncrequest::Error& error) {
    result.got_error = true;
    result.kind      = error.kind();
    if (error.is_Client()) {
        result.backend     = error.as_Client().error.backend;
        result.client_code = error.as_Client().error.code.to_primitive();
    }
}

auto fetch_text_request(ncrequest::Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        rsp = co_await session->get(req.try_clone().unwrap());
    if (rsp.is_err()) {
        auto error        = rstd::move(rsp).unwrap_err();
        result.got_error  = true;
        result.error_kind = error.kind();
        result.error      = to_string(rstd::format("{}", error));
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;

    auto text = co_await response->text();
    if (text.is_err()) {
        result.error = to_string(
            rstd::format("response text read failed: {}", text.unwrap_err()));
        co_return result;
    }

    result.code                = response_code(response);
    result.has_test_header     = response->header().contains("x-ncrequest-test"_str);
    result.initial_has_trailer = response->header().contains("x-ncrequest-trailer"_str);
    result.body                = text.unwrap();
    auto trailers              = response->trailers();
    if (trailers.is_some()) {
        result.trailer_count = (**trailers).get_all("x-ncrequest-trailer"_str).len().to_primitive();
    }
    auto set_cookies = response->set_cookies();
    if (set_cookies.is_err()) {
        result.error = "Set-Cookie parsing failed";
        co_return result;
    }
    auto cookies            = rstd::move(set_cookies).unwrap();
    result.set_cookie_count = cookies.len().to_primitive();
    if (! cookies.is_empty()) {
        result.first_set_cookie_name = to_string(cookies[usize()].cookie().name());
    }
    auto repeated = response->header().get_all("x-ncrequest-repeat"_str);
    for (const auto& value : repeated) {
        auto text_value = value->to_str().ok();
        if (text_value.is_none()) {
            result.error = "repeated response header is not UTF-8";
            co_return result;
        }
        if (! result.repeated_header_values.empty()) {
            result.repeated_header_values.push_back('|');
        }
        result.repeated_header_values.append(as_string_view(*text_value));
    }
    result.got_body = true;
    co_return result;
}

auto fetch_text(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    return fetch_text_request(rstd::move(session), make_request(url));
}

auto request_with_share(std::string url, const ncrequest::SessionShare& share)
    -> ncrequest::Request {
    auto request = make_request(url);
    auto options = RequestOptions {};
    options.share = Some(Share {Some(share.clone())});
    request.set_options(rstd::move(options));
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
    auto response = Option<ncrequest::Arc<ncrequest::Response>> {};
    {
        auto request = request_with_share(rstd::move(url), share);
        auto started = co_await session->get(request.try_clone().unwrap());
        if (started.is_err()) {
            result.error = "session returned error";
            co_return result;
        }
        response            = Some(rstd::move(started).unwrap());
        result.got_response = true;
    }

    auto text = co_await (*response)->text();
    if (text.is_err()) {
        result.error = "response text read failed";
        co_return result;
    }
    result.code            = response_code(*response);
    result.has_test_header = (*response)->header().contains("x-ncrequest-test"_str);
    result.body            = rstd::move(text).unwrap();
    result.got_body        = true;
    co_return result;
}

auto exercise_share(ncrequest::Arc<ncrequest::Session> session, std::string base,
                    PathBuf cookie_file, PathBuf fixture_file) -> ncrequest::coro<ShareResult> {
    auto result   = ShareResult {};
    auto shared   = ncrequest::SessionShare {};
    auto isolated = ncrequest::SessionShare {};

    result.default_set = co_await fetch_text(
        session.clone(), local_http_url(base, "/cookie/set?name=default_cookie&value=default"));
    auto shared_task   = spawn_local(fetch_text_request(
        session.clone(),
        request_with_share(local_http_url(base, "/cookie/set?name=shared_cookie&value=shared"),
                           shared)));
    auto isolated_task = spawn_local(fetch_text_request(
        session.clone(),
        request_with_share(local_http_url(base, "/cookie/set?name=isolated_cookie&value=isolated"),
                           isolated)));
    auto share_sets =
        co_await join(rstd::move(shared_task), rstd::move(isolated_task));
    result.share_set    = rstd::move(share_sets.get<0>()).unwrap();
    result.isolated_set = rstd::move(share_sets.get<1>()).unwrap();
    result.default_echo =
        co_await fetch_text(session.clone(), local_http_url(base, "/cookie/echo"));
    result.share_echo = co_await fetch_text_request(
        session.clone(), request_with_share(local_http_url(base, "/cookie/echo"), shared));
    result.isolated_echo = co_await fetch_text_request(
        rstd::move(session), request_with_share(local_http_url(base, "/cookie/echo"), isolated));

    auto second_session = ncrequest::Session::make();
    auto cloned         = shared.clone();
    result.cloned_echo  = co_await fetch_text_request(
        second_session.clone(), request_with_share(local_http_url(base, "/cookie/echo"), cloned));
    result.redirect_echo = co_await fetch_text_request(
        second_session.clone(),
        request_with_share(
            local_http_url(base, "/cookie/redirect-set?name=redirect_cookie&value=redirected"),
            cloned));
    result.temporary_request = co_await fetch_after_request_drop(
        second_session.clone(),
        local_http_url(base, "/cookie/slow-set?name=lifetime_cookie&value=alive"),
        cloned);
    result.canceled_request = co_await cancel_request(
        second_session.clone(), request_with_share(local_http_url(base, "/slow-stream"), cloned));
    result.timed_out_request = co_await timeout_request(
        second_session.clone(),
        request_with_share(local_http_url(base, "/slow-first-byte"), cloned));
    result.recovered_echo = co_await fetch_text_request(
        second_session.clone(), request_with_share(local_http_url(base, "/cookie/echo"), cloned));

    auto fixture = ncrequest::SessionShare {};
    fixture.load(fixture_file.as_path());
    result.fixture_echo = co_await fetch_text_request(
        second_session.clone(), request_with_share(local_http_url(base, "/cookie/echo"), fixture));

    cloned.save(cookie_file.as_path());
    auto persisted = ncrequest::SessionShare {};
    persisted.load(cookie_file.as_path());
    result.persisted_echo = co_await fetch_text_request(
        rstd::move(second_session),
        request_with_share(local_http_url(base, "/cookie/echo"), persisted));
    co_return result;
}

auto fetch_bytes(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
    auto        rsp = co_await session->get(req.try_clone().unwrap());
    if (rsp.is_err()) {
        result.error = "session returned error";
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().contains("x-ncrequest-test"_str);
    result.body            = string_from_bytes(rstd::move(bytes).unwrap());
    result.got_body        = true;
    co_return result;
}

auto post_text(ncrequest::Arc<ncrequest::Session> session, std::string url, std::string body)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
    auto        rsp = co_await session->post(req.try_clone().unwrap(), bytes_from_string(body));
    if (rsp.is_err()) {
        result.error = "session returned error";
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;

    auto text = co_await response->text();
    if (text.is_err()) {
        result.error = "response text read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().contains("x-ncrequest-test"_str);
    result.body            = text.unwrap();
    result.got_body        = true;
    co_return result;
}

auto post_bytes(ncrequest::Arc<ncrequest::Session> session, std::string url, std::string body)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
    auto        rsp = co_await session->post(req.try_clone().unwrap(), bytes_from_string(body));
    if (rsp.is_err()) {
        result.error = "session returned error";
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().contains("x-ncrequest-test"_str);
    result.body            = string_from_bytes(rstd::move(bytes).unwrap());
    result.got_body        = true;
    co_return result;
}

auto timeout_request(ncrequest::Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;
    auto timeout = Timeout {};
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
    timeout.transfer_timeout = i64(100);
#else
    timeout.low_speed        = i64(1);
    timeout.transfer_timeout = i64(1);
#endif
    auto options = req.options().clone();
    options.timeout = Some(timeout);
    req.set_options(rstd::move(options));
    auto rsp = co_await session->get(req.try_clone().unwrap());
    if (rsp.is_err()) {
        auto error = rstd::move(rsp).unwrap_err();
        record_error(result, error);
        co_return result;
    }
    result.got_response = true;

    auto text = co_await rstd::move(rsp).unwrap()->text();
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
    return timeout_request(rstd::move(session), make_request(url));
}

auto cancel_request(ncrequest::Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;

    auto rsp = co_await session->get(req.try_clone().unwrap());
    if (rsp.is_err()) {
        result.error = "session returned error before cancel";
        co_return result;
    }
    result.got_response = true;

    auto response = rstd::move(rsp).unwrap();
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
    return cancel_request(rstd::move(session), make_request(url));
}

#ifdef NCREQUEST_CLIENT_BACKEND_CURL
auto curl_slow_consumer(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
    auto        rsp = co_await session->get(req.try_clone().unwrap());
    if (rsp.is_err()) {
        result.error = "session returned error";
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;
    auto body = response->take_body().unwrap();
    co_await sleep(Duration::from_millis(u64(350)));
    result.finished_while_paused = response->is_finished();

    auto bytes = co_await body.collect();
    if (bytes.is_err()) {
        result.error = "response bytes read failed after pause";
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().contains("x-ncrequest-test"_str);
    result.body            = string_from_bytes(rstd::move(bytes).unwrap());
    result.got_body        = true;
    co_return result;
}

auto curl_streaming_upload(ncrequest::Arc<ncrequest::Session> session, std::string url,
                           std::string body) -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
    usize       offset {};
    usize       calls {};
    auto reader = BodyReader {};
    reader.size = Some(usize(body.size()));
    reader.callback    = [&body, &offset, &calls](byte* ptr, usize size) -> usize {
        ++calls;
        auto remaining = usize(body.size()) - offset;
        auto copied    = remaining;
        if (copied > size) copied = size;
        if (copied > usize(4096)) copied = usize(4096);
        if (copied == usize()) return usize();
        std::memcpy(ptr, body.data() + offset.to_primitive(), copied.to_primitive());
        offset += copied;
        return copied;
    };

    req.set_body(RequestBody::from_reader(rstd::move(reader)).unwrap());
    auto rsp = co_await session->post(rstd::move(req));
    if (rsp.is_err()) {
        result.error = "session returned error";
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;

    auto bytes = co_await response->bytes();
    if (bytes.is_err()) {
        result.error = "response bytes read failed";
        co_return result;
    }

    result.code                  = response_code(response);
    result.has_test_header       = response->header().contains("x-ncrequest-test"_str);
    result.body                  = string_from_bytes(rstd::move(bytes).unwrap());
    result.upload_callback_count = calls.to_primitive();
    result.got_body              = true;
    co_return result;
}
#endif

template<typename Start>
auto run_http(Start&& start) {
    auto session = ncrequest::Session::make();
    return block_on(start(rstd::move(session)));
}

template<typename Start>
auto run_http_multi_thread(Start&& start) {
    auto runtime = RuntimeBuilder::multi_thread()
                       .worker_threads(usize(2))
                       .build()
                       .unwrap();
    auto session = ncrequest::Session::make();
    return runtime.block_on(start(rstd::move(session)));
}

auto rstd_wait_yield() -> ncrequest::coro<int> {
    co_await yield_now();
    co_return 42;
}

} // namespace

TEST(http, HeaderCloneAndRequestReuseTypedOwner) {
    auto source = lihttpto::Headers {};
    ASSERT_TRUE(source.add("X-First"_str, "one"_str).is_ok());
    ASSERT_TRUE(source.add("Set-Cookie"_str, "a=1"_str).is_ok());
    ASSERT_TRUE(source.add("set-cookie"_str, "b=2"_str).is_ok());

    auto cloned = as<Clone>(source).clone();
    ASSERT_TRUE(cloned.set("X-First"_str, "changed"_str).is_ok());
    auto source_first = source.get("x-first"_str);
    auto cloned_first = cloned.get("x-first"_str);
    ASSERT_TRUE(source_first.is_some());
    ASSERT_TRUE(cloned_first.is_some());
    EXPECT_EQ(as_string_view(*(**source_first).to_str().ok()), "one");
    EXPECT_EQ(as_string_view(*(**cloned_first).to_str().ok()), "changed");

    auto request = ncrequest::Request {};
    request.update_header(source);
    EXPECT_EQ(request.header("x-first"), "one");
    EXPECT_EQ(request.header().get_all("set-cookie"_str).len().to_primitive(), 2u);
    ASSERT_TRUE(request.try_set_header("X-First"_str, "request"_str).is_ok());
    EXPECT_EQ(request.header("x-first"), "request");

    auto request_clone = request.try_clone().unwrap();
    ASSERT_TRUE(request_clone.try_set_header("X-First"_str, "clone"_str).is_ok());
    EXPECT_EQ(request.header("x-first"), "request");
    EXPECT_EQ(request_clone.header("x-first"), "clone");
}

TEST(http, RstdAsyncPollFuture) {
    auto value = block_on(rstd_wait_yield());
    EXPECT_EQ(value, 42);
}

TEST(http, ErrorModelVariants) {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    ncrequest::Error curl_error = rstd::into(curl::CURLcode::CURLE_COULDNT_CONNECT);
    EXPECT_EQ(curl_error.kind(), ncrequest::ErrorKind::Client);
    ASSERT_TRUE(curl_error.is_Client());
    EXPECT_EQ(curl_error.as_Client().error.backend, ncrequest::ClientBackend::Curl);
    EXPECT_EQ(curl_error.as_Client().error.code,
              static_cast<i32>(curl::CURLcode::CURLE_COULDNT_CONNECT));
    auto client_source = as<rstd::error::Error>(curl_error).source();
    ASSERT_TRUE(client_source.is_some());
    EXPECT_TRUE(rstd::error::is<ncrequest::ClientError>(*client_source));
    EXPECT_EQ(client_source->as_raw_ptr(), &curl_error.as_Client().error);
    EXPECT_EQ(to_string(rstd::format("{}", curl_error)), "client request failed");
    EXPECT_EQ(to_string(rstd::format("{}", *client_source)),
              curl::curl_easy_strerror(curl::CURLcode::CURLE_COULDNT_CONNECT));

    auto multi_error = ncrequest::CurlMultiError::Multi(curl::CURLMcode::CURLM_BAD_HANDLE);
    EXPECT_TRUE(as<rstd::error::Error>(multi_error).source().is_none());
    EXPECT_EQ(to_string(rstd::format("{}", multi_error)),
              curl::curl_multi_strerror(curl::CURLMcode::CURLM_BAD_HANDLE));
#else
    ncrequest::Error client = rstd::into(ncrequest::ClientError {
        .backend = ncrequest::ClientBackend::QtNetwork,
        .code    = i32(7),
        .message = "client error",
    });
    EXPECT_EQ(client.kind(), ncrequest::ErrorKind::Client);
    ASSERT_TRUE(client.is_Client());
    EXPECT_EQ(client.as_Client().error.backend, ncrequest::ClientBackend::QtNetwork);
    EXPECT_EQ(client.as_Client().error.code.to_primitive(), 7);
    auto client_source = as<rstd::error::Error>(client).source();
    ASSERT_TRUE(client_source.is_some());
    EXPECT_TRUE(rstd::error::is<ncrequest::ClientError>(*client_source));
    EXPECT_EQ(client_source->as_raw_ptr(), &client.as_Client().error);
    EXPECT_EQ(to_string(rstd::format("{}", client)), "client request failed");
    EXPECT_EQ(to_string(rstd::format("{}", *client_source)), "client error");
#endif

    auto io = IoError::from_kind(
        IoErrorKind { IoErrorKind::TimedOut });
    ncrequest::Error io_error = rstd::into(rstd::move(io));
    EXPECT_EQ(io_error.kind(), ncrequest::ErrorKind::Io);
    ASSERT_TRUE(io_error.is_Io());
    EXPECT_EQ(io_error.as_Io().error.kind(),
              (IoErrorKind { IoErrorKind::TimedOut }));
    auto io_source = as<rstd::error::Error>(io_error).source();
    ASSERT_TRUE(io_source.is_some());
    EXPECT_TRUE(rstd::error::is<IoError>(*io_source));
    EXPECT_EQ(io_source->as_raw_ptr(), &io_error.as_Io().error);
    EXPECT_EQ(to_string(rstd::format("{}", io_error)), "I/O request failed");
    EXPECT_EQ(to_string(rstd::format("{}", *io_source)), "timed out");

    auto canceled = ncrequest::Error::Canceled();
    EXPECT_EQ(canceled.kind(), ncrequest::ErrorKind::Canceled);

    auto unsupported = ncrequest::Error::Unsupported("unsupported capability");
    EXPECT_EQ(unsupported.kind(), ncrequest::ErrorKind::Unsupported);
    EXPECT_EQ(to_string(rstd::format("{}", unsupported)), "unsupported capability");
}

TEST(http, UnifiedOptionsInheritanceAndOverride) {
    auto defaults = SessionOptions {};
    defaults.timeout = Timeout {i64(2), i64(3), i64(4)};
    defaults.proxy = Proxy {Proxy::Type::SOCKS5, "127.0.0.1:1080"};
    defaults.tcp = Tcp {true, i64(12), i64(6)};
    defaults.tls.verify_certificate = false;
    defaults.share = Share {Some(ncrequest::SessionShare {})};
    auto request = make_request("http://localhost/");
    auto inherited = PreparedRequest::prepare(request.try_clone().unwrap(), defaults).unwrap();
    EXPECT_EQ(inherited.options().timeout().connect_timeout, i64(3));
    EXPECT_EQ(inherited.options().timeout().low_speed, i64(2));
    EXPECT_EQ(inherited.options().timeout().transfer_timeout, i64(4));
    EXPECT_EQ(inherited.options().proxy().type, Proxy::Type::SOCKS5);
    EXPECT_EQ(inherited.options().proxy().content, "127.0.0.1:1080");
    EXPECT_TRUE(inherited.options().tcp().keepalive);
    EXPECT_FALSE(inherited.options().tls().verify_certificate);
    EXPECT_TRUE(inherited.options().share().share.is_some());

    auto overrides = RequestOptions {};
    overrides.timeout = Some(Timeout {i64(), i64(), i64()});
    overrides.proxy = Some(Proxy {});
    overrides.tcp = Some(Tcp {});
    overrides.tls = Some(SSL {});
    overrides.share = Some(Share {});
    request.set_options(rstd::move(overrides));
    auto copy = request.try_clone().unwrap();
    auto effective = PreparedRequest::prepare(rstd::move(copy), defaults).unwrap();
    EXPECT_EQ(effective.options().timeout().transfer_timeout, i64());
    EXPECT_TRUE(effective.options().proxy().content.empty());
    EXPECT_FALSE(effective.options().tcp().keepalive);
    EXPECT_TRUE(effective.options().tls().verify_certificate);
    EXPECT_TRUE(effective.options().share().share.is_none());
    defaults.proxy.content.clear();
    EXPECT_EQ(inherited.options().proxy().content, "127.0.0.1:1080");
    EXPECT_FALSE(inherited.options().tls().verify_certificate);
    EXPECT_TRUE(request.options().share.is_some());
    EXPECT_TRUE(request.options().share->share.is_none());
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
    auto result =
        run_http([base, cookie_path = cookie_file.clone(), fixture_path = fixture_file.clone()](
                     auto session) mutable {
            return exercise_share(
                rstd::move(session), base, rstd::move(cookie_path), rstd::move(fixture_path));
        });
    auto persisted = read_file(cookie_file);
    remove_file(cookie_file);
    remove_file(fixture_file);

    auto assert_fetch = [](const char* name, const FetchResult& fetch) {
        SCOPED_TRACE(name);
        ASSERT_TRUE(fetch.got_response) << fetch.error;
        ASSERT_TRUE(fetch.got_body) << fetch.error;
        EXPECT_EQ(fetch.code, 200) << fetch.body;
    };
    assert_fetch("default_set", result.default_set);
    assert_fetch("share_set", result.share_set);
    assert_fetch("isolated_set", result.isolated_set);
    assert_fetch("default_echo", result.default_echo);
    assert_fetch("share_echo", result.share_echo);
    assert_fetch("isolated_echo", result.isolated_echo);
    assert_fetch("cloned_echo", result.cloned_echo);
    assert_fetch("temporary_request", result.temporary_request);
    assert_fetch("recovered_echo", result.recovered_echo);
    assert_fetch("fixture_echo", result.fixture_echo);
    assert_fetch("persisted_echo", result.persisted_echo);
    EXPECT_EQ(result.default_set.set_cookie_count, 1u);
    EXPECT_EQ(result.default_set.first_set_cookie_name, "default_cookie");
    EXPECT_EQ(result.share_set.set_cookie_count, 1u);
    EXPECT_EQ(result.share_set.first_set_cookie_name, "shared_cookie");
    EXPECT_EQ(result.isolated_set.set_cookie_count, 1u);
    EXPECT_EQ(result.isolated_set.first_set_cookie_name, "isolated_cookie");
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
    EXPECT_NE(result.recovered_echo.body.find("redirect_cookie=redirected"), std::string::npos);
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
        return fetch_text(rstd::move(session), url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(http, LocalHttpGetTextRstdMultiThreadRuntime) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http_multi_thread([url = local_http_url(base, "/text")](auto session) {
        return fetch_text(rstd::move(session), url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(http, LocalHttpRedirectUsesFinalMessageHead) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/redirect")](auto session) {
        return fetch_text(rstd::move(session), url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(http, LocalHttpPreservesRepeatedRequestAndResponseHeaders) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto request_result =
        run_http([url = local_http_url(base, "/headers/request-repeat")](auto session) {
            auto request = make_request(url);
            auto headers = lihttpto::Headers {};
            (void)headers.add("X-Ncrequest-Repeat"_str, "one"_str);
            (void)headers.add("X-Ncrequest-Repeat"_str, "two"_str);
            request.update_header(headers);
            return fetch_text_request(rstd::move(session), rstd::move(request));
        });
#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
    ASSERT_TRUE(request_result.got_error) << request_result.error;
    EXPECT_EQ(request_result.error_kind, ncrequest::ErrorKind::Unsupported);
#else
    ASSERT_TRUE(request_result.got_body) << request_result.error;
    EXPECT_EQ(request_result.body, "one|two\n");
#endif

    auto response_result =
        run_http([url = local_http_url(base, "/headers/response-repeat")](auto session) {
            return fetch_text(rstd::move(session), url);
        });
    ASSERT_TRUE(response_result.got_body) << response_result.error;
    EXPECT_EQ(response_result.repeated_header_values, "one|two");
    EXPECT_EQ(response_result.set_cookie_count, 2u);
    EXPECT_EQ(response_result.first_set_cookie_name, "first");
}

TEST(http, LocalHttpKeepsTrailersSeparateFromInitialHeaders) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/headers/trailer")](auto session) {
        return fetch_text(rstd::move(session), url);
    });
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.body, "body");
    EXPECT_FALSE(result.initial_has_trailer);
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    EXPECT_EQ(result.trailer_count, 1u);
#else
    EXPECT_EQ(result.trailer_count, 0u);
#endif
}

TEST(http, LocalHttpLargeBody) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/large")](auto session) {
        return fetch_text(rstd::move(session), url);
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
        return fetch_text(rstd::move(session), url);
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
        return fetch_text(rstd::move(session), url);
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
        return post_text(rstd::move(session), url, payload);
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
        return fetch_bytes(rstd::move(session), url);
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
        return post_bytes(rstd::move(session), url, body);
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
        return fetch_timeout(rstd::move(session), url);
    });
    ASSERT_TRUE(result.got_error) << result.error;
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Client);
    EXPECT_EQ(result.backend, ncrequest::ClientBackend::QtNetwork);
#else
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Client);
    EXPECT_EQ(result.backend, ncrequest::ClientBackend::Curl);
    EXPECT_EQ(result.client_code, static_cast<int>(curl::CURLcode::CURLE_OPERATION_TIMEDOUT));
#endif
}

TEST(http, LocalHttpCancel) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/slow-stream")](auto session) {
        return fetch_then_cancel(rstd::move(session), url);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_error) << result.error;
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Canceled);
}

TEST(http, LocalHttpCurlBackpressure) {
#ifndef NCREQUEST_CLIENT_BACKEND_CURL
    GTEST_SKIP() << "curl-only bounded receive queue test";
#else
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/slow-stream")](auto session) {
        return curl_slow_consumer(rstd::move(session), url);
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
        return curl_streaming_upload(rstd::move(session), url, payload);
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, payload);
    EXPECT_GT(result.upload_callback_count, 1u);
#endif
}

static_assert(lihttpto::BodySource<ncrequest::ResponseBody>);
static_assert(!std::is_copy_constructible_v<ncrequest::ResponseBody>);

namespace
{
auto response_before_body(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto started = steady_clock::now();
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto elapsed = steady_clock::now() - started;
    auto value = rstd::move(response).unwrap();
    auto correct = value->head().status.value() == u16(200) &&
                   value->header().contains("x-ncrequest-test"_str) &&
                   !value->is_finished() && elapsed < milliseconds(700);
    auto bytes = co_await value->bytes();
    co_return correct && bytes.is_ok() && string_from_bytes(*bytes) == "delayed body";
}

auto owned_body(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto body = Option<ncrequest::ResponseBody> {};
    {
        auto response = co_await session->get(make_request(url));
        if (response.is_err()) co_return false;
        auto value = rstd::move(response).unwrap();
        body = Some(value->take_body().unwrap());
        if (value->take_body().is_ok()) co_return false;
    }
    std::string contents;
    std::size_t chunks = 0;
    for (;;) {
        auto part = co_await body->next();
        if (part.is_err()) co_return false;
        if (part->is_none()) break;
        ++chunks;
        contents += string_from_bytes(**part);
    }
    auto after_eof = co_await body->next();
    auto second_collect = co_await body->collect();
    co_return chunks > 1 && contents == download_body() &&
              after_eof.is_err() && after_eof.unwrap_err().is_InvalidState() &&
              second_collect.is_err() && second_collect.unwrap_err().is_InvalidState();
}

auto limited_body(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value = rstd::move(response).unwrap();
    auto bytes = co_await value->bytes(usize(128));
    if (bytes.is_ok() || !bytes.unwrap_err().is_Protocol()) co_return false;
    auto second = co_await value->text();
    co_return bytes.unwrap_err().as_Protocol().kind == ncrequest::ProtocolError::BodyTooLarge &&
              second.is_err() && second.unwrap_err().is_InvalidState();
}

struct StreamingSink {
    using Error = ncrequest::Error;
    std::string contents;
    std::size_t writes {};
    bool fail {false};
    auto write(Bytes bytes) -> ncrequest::coro<ncrequest::Result<empty>> {
        if (fail) co_return Err(Error::InvalidState("sink rejected chunk"));
        ++writes;
        contents += string_from_bytes(bytes);
        co_return Ok(empty {});
    }
};

auto transfer_body_to_sink(ncrequest::Arc<ncrequest::Session> session, std::string url, bool fail)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value = rstd::move(response).unwrap();
    StreamingSink sink;
    sink.fail = fail;
    auto written = co_await value->read_to_stream(sink);
    if (fail) co_return written.is_err() && written.unwrap_err().is_Sink();
    co_return written.is_ok() && written->to_primitive() == download_body().size() &&
              sink.writes > 1 && sink.contents == download_body();
}

auto body_drop_cancels(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value = rstd::move(response).unwrap();
    {
        auto body = value->take_body();
        if (body.is_err()) co_return false;
    }
    for (int i = 0; i < 50 && !value->is_finished(); ++i)
        co_await sleep(Duration::from_millis(u64(10)));
    co_return value->is_finished();
}
}

TEST(http, LocalHttpHeaderBeforeBody) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/delayed-body")](auto session) {
        return response_before_body(rstd::move(session), url);
    }));
}

TEST(http, LocalHttpOwnedSingleConsumerBody) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/download.bin")](auto session) {
        return owned_body(rstd::move(session), url);
    }));
}

TEST(http, LocalHttpBodyCollectionLimit) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/download.bin")](auto session) {
        return limited_body(rstd::move(session), url);
    }));
}

TEST(http, LocalHttpBodySink) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    for (bool fail : {false, true}) {
        EXPECT_TRUE(run_http([url = local_http_url(base, "/download.bin"), fail](auto session) {
            return transfer_body_to_sink(rstd::move(session), url, fail);
        }));
    }
}

TEST(http, LocalHttpBodyDropCancels) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/delayed-body")](auto session) {
        return body_drop_cancels(rstd::move(session), url);
    }));
}

TEST(http, LocalHttpInformationalHead) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto result = run_http([url = local_http_url(base, "/informational")](auto session) {
        return fetch_text(rstd::move(session), url);
    });
    EXPECT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_EQ(result.body, "final response");
}

TEST(http, LocalHttpEmptyBodyTrailer) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto result = run_http([url = local_http_url(base, "/empty-trailer")](auto session) {
        return fetch_text(rstd::move(session), url);
    });
    EXPECT_TRUE(result.got_body) << result.error;
    EXPECT_TRUE(result.body.empty());
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    EXPECT_EQ(result.trailer_count, 1u);
#endif
}

namespace
{
auto concurrent_body_read(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto body = (*response)->take_body().unwrap();
    auto first = spawn_local(body.next());
    co_await sleep(Duration::from_millis(u64(20)));
    auto second = co_await body.next();
    if (second.is_ok() || !second.unwrap_err().is_InvalidState()) co_return false;
    first.abort();
    auto aborted = co_await rstd::move(first);
    auto after_abort = co_await body.next();
    co_return aborted.is_err() && after_abort.is_err() && after_abort.unwrap_err().is_InvalidState();
}

auto close_session(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto pending = spawn_local(session->get(make_request(url)));
    co_await sleep(Duration::from_millis(u64(20)));
    session->close();
    auto joined = co_await rstd::move(pending);
    if (joined.is_err() || joined->is_ok() || !joined->unwrap_err().is_Canceled()) co_return false;
    auto rejected = co_await session->get(make_request(url));
    co_return rejected.is_err() && rejected.unwrap_err().is_Canceled();
}

auto send_after_session_drop(std::string url) -> ncrequest::coro<bool> {
    auto pending = [url] {
        auto session = ncrequest::Session::make();
        return session->get(make_request(url));
    }();
    auto response = co_await rstd::move(pending);
    co_return response.is_err() && response.unwrap_err().is_Canceled();
}
}

TEST(http, LocalHttpConcurrentBodyReaderAndAbort) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/delayed-body")](auto session) {
        return concurrent_body_read(rstd::move(session), url);
    }));
}

TEST(http, LocalHttpSessionClose) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/delay")](auto session) {
        return close_session(rstd::move(session), url);
    }));
    EXPECT_TRUE(block_on(send_after_session_drop(local_http_url(base, "/text"))));
}

namespace
{
template<class T>
concept NativeSessionAccess = requires(T& value) { value.channel(); };
template<class T>
concept NativeResponseAccess = requires(T& value) { value.pause_recv(true); };
static_assert(!NativeSessionAccess<ncrequest::Session>);
static_assert(!NativeResponseAccess<ncrequest::Response>);

auto truncated_body(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto body = (*response)->take_body().unwrap();
    std::string partial;
    for (;;) {
        auto next = co_await body.next();
        if (next.is_err()) co_return partial == "partial" && next.unwrap_err().is_Client();
        if (next->is_none()) co_return false;
        partial += string_from_bytes(**next);
    }
}

auto zero_limit_empty_body(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto empty = co_await (*response)->bytes(usize());
    co_return empty.is_ok() && empty->size() == usize();
}
}

TEST(http, LocalHttpBodyErrorIsNotEof) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/truncated-body")](auto session) {
        return truncated_body(rstd::move(session), url);
    }));
    EXPECT_TRUE(run_http([url = local_http_url(base, "/empty")](auto session) {
        return zero_limit_empty_body(rstd::move(session), url);
    }));
}

namespace {
auto send_method(ncrequest::Arc<ncrequest::Session> session, std::string url,
                 std::string method, int body_kind) -> ncrequest::coro<bool> {
    auto payload = body_kind == 2 ? std::string("one\0two", 7) : std::string {};
    auto pending = [&] {
        auto request = make_request(url);
        request.try_set_method(as_rstd_str(method)).unwrap();
        if (body_kind != 0) request.set_body(bytes_from_string(payload));
        return session->send(rstd::move(request));
    }();
    auto result = co_await rstd::move(pending);
    if (result.is_err()) co_return false;
    auto response = rstd::move(result).unwrap();
    if (response_code(response) != 200) co_return false;
    auto bytes = co_await response->bytes();
    if (bytes.is_err()) co_return false;
    auto received = string_from_bytes(rstd::move(bytes).unwrap());
    if (method == "HEAD")
        co_return received.empty() && response->header().contains("x-ncrequest-method"_str);
    co_return received == method + "\n" + payload;
}

auto reject_invalid_request(ncrequest::Arc<ncrequest::Session> session)
    -> ncrequest::coro<bool> {
    auto missing_url = co_await session->send(ncrequest::Request {});
    if (missing_url.is_ok() || !missing_url.unwrap_err().is_InvalidState()) co_return false;
    auto head = make_request("http://127.0.0.1:1/");
    head.try_set_method("HEAD"_str).unwrap();
    head.set_body(bytes_from_string("body"));
    auto response = co_await session->send(rstd::move(head));
    co_return response.is_err() && response.unwrap_err().is_InvalidState();
}

auto abort_owned_upload(ncrequest::Arc<ncrequest::Session> session, std::string base)
    -> ncrequest::coro<bool> {
    auto pending = [&] {
        auto request = make_request(local_http_url(base, "/slow-upload"));
        request.try_set_method("POST"_str).unwrap();
        request.set_body(bytes_from_string(std::string(8 * 1024 * 1024, 'x')));
        return spawn_local(session->send(rstd::move(request)));
    }();
    co_await sleep(Duration::from_millis(u64(20)));
    pending.abort();
    auto canceled = co_await rstd::move(pending);
    if (canceled.is_ok()) co_return false;
    co_return co_await send_method(rstd::move(session), local_http_url(base, "/method"), "POST", 2);
}
}

TEST(http, RequestMethodAndOwnedBody) {
    auto request = make_request("http://127.0.0.1/");
    EXPECT_EQ(as_string_view(request.method().as_ref()), "GET");
    ASSERT_TRUE(request.try_set_method("REPORT"_str).is_ok());
    for (auto invalid : {"", "GET /", "POST\r\nX: injected"}) {
        EXPECT_TRUE(request.try_set_method(as_rstd_str(invalid)).is_err());
        EXPECT_EQ(as_string_view(request.method().as_ref()), "REPORT");
    }
    request.set_method(lihttpto::Method::parse("PATCH"_str).unwrap());
    request.set_body(bytes_from_string("owned"));
    auto cloned = request.try_clone().unwrap();
    request.clear_body();
    EXPECT_TRUE(request.body().bytes().is_none());
    ASSERT_TRUE(cloned.body().bytes().is_some());
    EXPECT_EQ(as_string_view(cloned.method().as_ref()), "PATCH");
    EXPECT_EQ(string_from_bytes(Bytes::copy_from_slice(cloned.body().bytes()->as_slice())), "owned");
    EXPECT_TRUE(cloned.validate().is_ok());
    auto reader = BodyReader {[](byte*, usize) { return usize(); }, Some(usize())};
    cloned.set_body(RequestBody::from_reader(rstd::move(reader)).unwrap());
    EXPECT_TRUE(cloned.body().bytes().is_none());
    EXPECT_TRUE(cloned.try_clone().is_err());
    EXPECT_TRUE(cloned.validate().unwrap_err().is_Unsupported());
    cloned.clear_body();
    EXPECT_TRUE(cloned.try_clone().is_ok());
}

TEST(http, LocalHttpMethodAndBodyMatrix) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    for (auto method : {"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "REPORT", "HEAD"}) {
        for (int body_kind = 0; body_kind != (std::string_view(method) == "HEAD" ? 2 : 3); ++body_kind) {
            SCOPED_TRACE(std::string(method) + " body " + std::to_string(body_kind));
            EXPECT_TRUE(run_http([url = local_http_url(base, "/method"), method, body_kind](auto session) {
                return send_method(rstd::move(session), url, method, body_kind);
            }));
        }
    }
}

TEST(http, RequestValidationBeforeNetwork) {
    EXPECT_TRUE(run_http([](auto session) {
        return reject_invalid_request(rstd::move(session));
    }));
}

TEST(http, LocalHttpAbortOwnedUpload) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([base](auto session) {
        return abort_owned_upload(rstd::move(session), base);
    }));
}

namespace {
auto socket_endpoint(std::string_view path) -> Endpoint {
    return Endpoint::unix_socket(PathBuf::from(as_rstd_str(path))).unwrap();
}

auto socket_path(const Endpoint& endpoint) -> std::string {
    auto path = endpoint.socket_path().unwrap();
    return std::string(as_string_view(path.to_str().unwrap()));
}

auto endpoint_request(std::string url, Endpoint endpoint) -> ncrequest::Request {
    auto request = make_request(url);
    auto options = RequestOptions {};
    options.endpoint = Some(rstd::move(endpoint));
    request.set_options(rstd::move(options));
    return request;
}

#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
auto local_socket_path() -> std::string {
    auto* path = std::getenv("NCREQUEST_TEST_UNIX_SOCKET");
    return path == nullptr ? std::string {} : std::string(path);
}
#endif
}

TEST(http, EndpointOptionsInheritanceAndSnapshot) {
    auto defaults = SessionOptions {};
    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    auto request = make_request("http://podman.invalid/v6/libpod/info?detail=1");
    auto inherited = PreparedRequest::prepare(request.try_clone().unwrap(), defaults).unwrap();
    EXPECT_EQ(socket_path(inherited.options().endpoint()), "/tmp/session.sock");
    EXPECT_EQ(inherited.request().url(), "http://podman.invalid/v6/libpod/info?detail=1");

    auto override = RequestOptions {};
    override.endpoint = Some(socket_endpoint("/tmp/request.sock"));
    request.set_options(rstd::move(override));
    auto cloned = request.try_clone().unwrap();
    auto selected = PreparedRequest::prepare(request.try_clone().unwrap(), defaults).unwrap();
    defaults.endpoint = Endpoint::network();
    request.set_options(RequestOptions {});
    EXPECT_EQ(socket_path(selected.options().endpoint()), "/tmp/request.sock");
    EXPECT_EQ(socket_path(*cloned.options().endpoint), "/tmp/request.sock");
    EXPECT_EQ(socket_path(inherited.options().endpoint()), "/tmp/session.sock");

    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    auto network = endpoint_request("http://localhost/", Endpoint::network());
    auto reset = PreparedRequest::prepare(rstd::move(network), defaults).unwrap();
    EXPECT_TRUE(reset.options().endpoint().socket_path().is_none());
}

TEST(http, EndpointInvalidPathsAndConflicts) {
    EXPECT_TRUE(Endpoint::unix_socket(PathBuf {}).is_err());
    EXPECT_TRUE(Endpoint::unix_socket(PathBuf::from("/tmp/one\0two"_str)).is_err());
    auto defaults = SessionOptions {};
    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    auto request = make_request("http://localhost/");
    auto overrides = RequestOptions {};
    overrides.proxy = Some(Proxy {Proxy::Type::HTTP, "http://127.0.0.1:1"});
    request.set_options(overrides.clone());
    auto proxy = PreparedRequest::prepare(request.try_clone().unwrap(), defaults);
    ASSERT_TRUE(proxy.is_err());
    EXPECT_TRUE(proxy.unwrap_err().is_InvalidState());
    overrides.proxy = Some(Proxy {});
    overrides.tcp = Some(Tcp {true, i64(120), i64(60)});
    request.set_options(overrides.clone());
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_err());
    overrides.tcp = Some(Tcp {});
    request.set_options(rstd::move(overrides));
    EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), defaults).is_ok());
}

TEST(http, LocalHttpUnixEndpoint) {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    auto path = local_socket_path();
    if (path.empty()) GTEST_SKIP();
    auto defaults = SessionOptions {};
    defaults.endpoint = socket_endpoint(path);
    auto session = ncrequest::Session::make(rstd::move(defaults));
    auto result = block_on(fetch_text_request(session.clone(),
        make_request("http://podman.invalid:8087/endpoint?detail=1")));
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.body, "unix\npodman.invalid:8087\n/endpoint?detail=1\n");

    auto network_url = local_http_url(local_http_base_url(), "/endpoint?network=1");
    auto reset = block_on(fetch_text_request(session.clone(),
        endpoint_request(network_url, Endpoint::network())));
    ASSERT_TRUE(reset.got_body) << reset.error;
    EXPECT_EQ(reset.body.substr(0, 4), "tcp\n");

    auto failed = block_on(fetch_text_request(session.clone(),
        endpoint_request("http://podman.invalid/text", socket_endpoint(path + ".missing"))));
    EXPECT_TRUE(failed.got_error);
    EXPECT_EQ(failed.error_kind, ncrequest::ErrorKind::Client);

    auto recovered = block_on(fetch_text_request(rstd::move(session),
        make_request("http://podman.invalid/text")));
    ASSERT_TRUE(recovered.got_body) << recovered.error;
#else
    GTEST_SKIP() << "curl-only Unix socket runtime test";
#endif
}

TEST(http, LocalHttpUnixRequestOverride) {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    auto path = local_socket_path();
    if (path.empty()) GTEST_SKIP();
    auto result = run_http([path](auto session) {
        return fetch_text_request(rstd::move(session),
            endpoint_request("http://not-resolved.invalid/endpoint", socket_endpoint(path)));
    });
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.body, "unix\nnot-resolved.invalid\n/endpoint\n");
#else
    GTEST_SKIP() << "curl-only Unix socket runtime test";
#endif
}

TEST(http, UnifiedOptionsValidationAndInheritedConflicts) {
    auto defaults = SessionOptions {};
    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    defaults.proxy.content = "http://127.0.0.1:1";
    auto request = make_request("http://localhost/");
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_err());
    auto options = RequestOptions {};
    options.proxy = Some(Proxy {});
    request.set_options(options.clone());
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_ok());
    options.timeout = Some(Timeout {i64(-1), i64(3), i64(0)});
    request.set_options(options.clone());
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_err());
    options.timeout = None();
    options.tcp = Some(Tcp {false, i64(-1), i64(0)});
    request.set_options(rstd::move(options));
    EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), defaults).is_err());
}

TEST(http, RequestBodyReaderOwnership) {
    EXPECT_TRUE(RequestBody::from_reader(BodyReader {}).is_err());
    auto reader = BodyReader {[](byte*, usize) { return usize(); }, Some(usize())};
    auto request = make_request("http://localhost/");
    request.try_set_method("POST"_str).unwrap();
    request.set_body(RequestBody::from_reader(rstd::move(reader)).unwrap());
    EXPECT_TRUE(request.body().reader().is_some());
    EXPECT_TRUE(request.body().reader()->size.is_some());
    EXPECT_EQ(*request.body().reader()->size, usize());
    EXPECT_TRUE(request.try_clone().is_err());
    request.set_body(bytes_from_string("replacement"));
    EXPECT_TRUE(request.body().reader().is_none());
    EXPECT_TRUE(request.try_clone().is_ok());
}

namespace {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
auto reader_upload(ncrequest::Arc<ncrequest::Session> session, std::string url,
                   bool known_empty) -> ncrequest::coro<bool> {
    std::size_t calls = 0;
    auto reader = BodyReader {};
    if (known_empty) reader.size = Some(usize());
    reader.callback = [&calls, position = std::size_t {}](byte* buffer, usize capacity) mutable {
        ++calls;
        const std::string payload = "callback payload";
        auto count = std::min(payload.size() - position, capacity.to_primitive());
        std::memcpy(buffer, payload.data() + position, count);
        position += count;
        return usize(count);
    };
    auto request = make_request(url);
    request.set_body(RequestBody::from_reader(rstd::move(reader)).unwrap());
    auto response = co_await session->post(rstd::move(request));
    if (response.is_err()) co_return false;
    auto bytes = co_await (*response)->bytes();
    if (bytes.is_err()) co_return false;
    auto body = string_from_bytes(*bytes);
    co_return known_empty ? body.empty() && calls == 0 : body == "callback payload" && calls > 0;
}
#endif
}

TEST(http, LocalHttpReaderLength) {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    for (bool empty : {false, true}) {
        EXPECT_TRUE(run_http([url = local_http_url(base, "/body-reader"), empty](auto session) {
            return reader_upload(rstd::move(session), url, empty);
        }));
    }
#else
    GTEST_SKIP() << "Qt Network does not support body readers";
#endif
}
