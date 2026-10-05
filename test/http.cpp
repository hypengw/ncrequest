#include <rstd/test/gtest.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <rstd/enum.hpp>
#include <string>
#include <string_view>
#include <type_traits>
import ncrequest;
#if ! defined(LITO_FEAT_QT)
import ncrequest.curl;
#endif
import rstd;

using namespace rstd::literals;
using namespace rstd::prelude;
using rstd::sync::Arc;
using IoError     = rstd::io::error::Error;
using IoErrorKind = rstd::io::error::ErrorKind;
using ncrequest::EffectiveOptions;
using ncrequest::Endpoint;
using ncrequest::LowSpeedOptions;
using ncrequest::PreparedRequest;
using ncrequest::RedirectOptions;
using ncrequest::RequestOptions;
using ncrequest::SessionOptions;
using ncrequest::TimeoutLimit;
using ncrequest::TlsClientIdentity;
using Proxy   = ncrequest::ProxyOptions;
using Share   = ncrequest::ShareOptions;
using SSL     = ncrequest::TlsOptions;
using Tcp     = ncrequest::TcpOptions;
using Timeout = ncrequest::TimeoutOptions;
using ncrequest::BodyReader;
using ncrequest::RequestBody;
using rstd::async::block_on;
using rstd::async::join;
using rstd::async::RuntimeBuilder;
using rstd::async::sleep;
using rstd::async::spawn_local;
using rstd::async::yield_now;
using rstd::bytes::Bytes;
using rstd::env::temp_dir;
using rstd::fs::read;
using rstd::fs::write;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::time::Duration;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

static_assert(! std::is_copy_constructible_v<ncrequest::Session>);
static_assert(! std::is_copy_assignable_v<ncrequest::Session>);
static_assert(! std::is_copy_constructible_v<ncrequest::Response>);
static_assert(! std::is_copy_assignable_v<ncrequest::Response>);
static_assert(! std::is_copy_constructible_v<ncrequest::ResponseBody>);
static_assert(! std::is_copy_assignable_v<ncrequest::ResponseBody>);
static_assert(std::is_move_constructible_v<ncrequest::ResponseBody>);
static_assert(std::is_move_assignable_v<ncrequest::ResponseBody>);

namespace
{

auto as_rstd_str(std::string_view value) -> ref<str> {
    return rstd::str_::from_utf8(
               slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(value.data()),
                                         usize(value.size())))
        .unwrap();
}

struct FetchResult {
    int                      client_code { 0 };
    ncrequest::ProtocolError protocol_error { ncrequest::ProtocolError::InvalidStatusLine };
    bool                     got_response { false };
    bool                     got_body { false };
    bool                     got_error { false };
    int                      code { 0 };
    bool                     has_test_header { false };
    std::size_t              set_cookie_count { 0 };
    bool                     finished_while_paused { false };
    std::size_t              upload_callback_count { 0 };
    std::size_t              trailer_count { 0 };
    bool                     initial_has_trailer { false };
    std::string              body;
    std::string              first_set_cookie_name;
    std::string              repeated_header_values;
    std::string              error;
    ncrequest::ErrorKind     error_kind { ncrequest::ErrorKind::InvalidState };
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
    auto bytes =
        slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(body.data()), usize(body.size()));
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

auto response_code(const Arc<ncrequest::Response>& rsp) -> int {
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

auto fetch_text_request(Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        rsp = co_await session->send(rstd::move(req));
    if (rsp.is_err()) {
        auto error        = rstd::move(rsp).unwrap_err();
        result.got_error  = true;
        result.error_kind = error.kind();
        if (error.is_Protocol()) result.protocol_error = error.as_Protocol().kind;
        if (error.is_Client()) result.client_code = error.as_Client().error.code.to_primitive();
        auto message = rstd::format("{}", error);
        result.error.assign(reinterpret_cast<const char*>(message.data()),
                            message.len().to_primitive());
        co_return result;
    }

    auto response       = rstd::move(rsp).unwrap();
    result.got_response = true;

    auto text = co_await response->text();
    if (text.is_err()) {
        auto error        = rstd::move(text).unwrap_err();
        result.got_error  = true;
        result.error_kind = error.kind();
        if (error.is_Protocol()) result.protocol_error = error.as_Protocol().kind;
        if (error.is_Client()) result.client_code = error.as_Client().error.code.to_primitive();
        auto message = rstd::format("response text read failed: {}", error);
        result.error.assign(reinterpret_cast<const char*>(message.data()),
                            message.len().to_primitive());
        co_return result;
    }

    result.code                = response_code(response);
    result.has_test_header     = response->header().contains("x-ncrequest-test"_str);
    result.initial_has_trailer = response->header().contains("x-ncrequest-trailer"_str);
    result.body.assign(reinterpret_cast<const char*>(text->data()), text->len().to_primitive());
    auto trailers = response->trailers();
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
        auto name = cookies[usize()].cookie().name();
        result.first_set_cookie_name.assign(reinterpret_cast<const char*>(name.data()),
                                            name.size().to_primitive());
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
        result.repeated_header_values.append(reinterpret_cast<const char*>(text_value->data()),
                                             text_value->size().to_primitive());
    }
    result.got_body = true;
    co_return result;
}

auto fetch_text(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<FetchResult> {
    return fetch_text_request(rstd::move(session), make_request(url));
}

auto request_with_share(std::string url, const ncrequest::SessionShare& share)
    -> ncrequest::Request {
    auto request     = make_request(url);
    auto options     = RequestOptions {};
    options.share    = Some(Share { Some(share.clone()) });
    options.redirect = Some(RedirectOptions::same_origin());
    request.set_options(rstd::move(options));
    return request;
}

auto cancel_request(Arc<ncrequest::Session>, ncrequest::Request) -> ncrequest::coro<ErrorResult>;
auto timeout_request(Arc<ncrequest::Session>, ncrequest::Request) -> ncrequest::coro<ErrorResult>;

auto fetch_after_request_drop(Arc<ncrequest::Session> session, std::string url,
                              const ncrequest::SessionShare& share)
    -> ncrequest::coro<FetchResult> {
    auto result   = FetchResult {};
    auto response = Option<Arc<ncrequest::Response>> {};
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
    result.body.assign(reinterpret_cast<const char*>(text->data()), text->len().to_primitive());
    result.got_body = true;
    co_return result;
}

auto exercise_share(Arc<ncrequest::Session> session, std::string base, PathBuf cookie_file,
                    PathBuf fixture_file) -> ncrequest::coro<ShareResult> {
    auto result   = ShareResult {};
    auto shared   = ncrequest::SessionShare::make().unwrap();
    auto isolated = ncrequest::SessionShare::make().unwrap();

    result.default_set = co_await fetch_text(
        session.clone(), local_http_url(base, "/cookie/set?name=default_cookie&value=default"));
    auto shared_task    = spawn_local(fetch_text_request(
        session.clone(),
        request_with_share(local_http_url(base, "/cookie/set?name=shared_cookie&value=shared"),
                           shared)));
    auto isolated_task  = spawn_local(fetch_text_request(
        session.clone(),
        request_with_share(local_http_url(base, "/cookie/set?name=isolated_cookie&value=isolated"),
                           isolated)));
    auto share_sets     = co_await join(rstd::move(shared_task), rstd::move(isolated_task));
    result.share_set    = rstd::move(share_sets.get<0>()).unwrap();
    result.isolated_set = rstd::move(share_sets.get<1>()).unwrap();
    result.default_echo =
        co_await fetch_text(session.clone(), local_http_url(base, "/cookie/echo"));
    result.share_echo = co_await fetch_text_request(
        session.clone(), request_with_share(local_http_url(base, "/cookie/echo"), shared));
    result.isolated_echo = co_await fetch_text_request(
        rstd::move(session), request_with_share(local_http_url(base, "/cookie/echo"), isolated));

    auto second_session = ncrequest::Session::make().unwrap();
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

    auto fixture = ncrequest::SessionShare::make().unwrap();
    fixture.load(fixture_file.as_path()).unwrap();
    result.fixture_echo = co_await fetch_text_request(
        second_session.clone(), request_with_share(local_http_url(base, "/cookie/echo"), fixture));

    cloned.save(cookie_file.as_path()).unwrap();
    auto persisted = ncrequest::SessionShare::make().unwrap();
    persisted.load(cookie_file.as_path()).unwrap();
    result.persisted_echo = co_await fetch_text_request(
        rstd::move(second_session),
        request_with_share(local_http_url(base, "/cookie/echo"), persisted));
    co_return result;
}

auto fetch_bytes(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<FetchResult> {
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

auto post_text(Arc<ncrequest::Session> session, std::string url, std::string body)
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
    result.body.assign(reinterpret_cast<const char*>(text->data()), text->len().to_primitive());
    result.got_body = true;
    co_return result;
}

auto post_bytes(Arc<ncrequest::Session> session, std::string url, std::string body)
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

auto timeout_request(Arc<ncrequest::Session> session, ncrequest::Request req)
    -> ncrequest::coro<ErrorResult> {
    ErrorResult result;
    auto        timeout = Timeout {};
    timeout.total       = TimeoutLimit::after(Duration::from_millis(u64(100)));
    auto options        = req.options().clone();
    options.timeout     = Some(timeout);
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

auto fetch_timeout(Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    return timeout_request(rstd::move(session), make_request(url));
}

auto cancel_request(Arc<ncrequest::Session> session, ncrequest::Request req)
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

auto fetch_then_cancel(Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<ErrorResult> {
    return cancel_request(rstd::move(session), make_request(url));
}

#ifndef LITO_FEAT_QT
auto curl_slow_consumer(Arc<ncrequest::Session> session, std::string url)
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
    auto body           = response->take_body().unwrap();
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

auto curl_streaming_upload(Arc<ncrequest::Session> session, std::string url, std::string body)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
    usize       offset {};
    usize       calls {};
    auto        reader = BodyReader {};
    reader.size        = Some(usize(body.size()));
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
    auto session = ncrequest::Session::make().unwrap();
    return block_on(start(rstd::move(session)));
}

template<typename Start>
auto run_http_multi_thread(Start&& start) {
    auto runtime = RuntimeBuilder::multi_thread().worker_threads(usize(2)).build().unwrap();
    auto session = ncrequest::Session::make().unwrap();
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
    EXPECT_TRUE(*(**source_first).to_str().ok() == "one"_str);
    EXPECT_TRUE(*(**cloned_first).to_str().ok() == "changed"_str);

    auto request = ncrequest::Request {};
    request.update_header(source);
    EXPECT_TRUE(request.header("x-first"_str).unwrap()->to_str().unwrap() == "one"_str);
    EXPECT_EQ(request.header().get_all("set-cookie"_str).len().to_primitive(), 2u);
    ASSERT_TRUE(request.try_set_header("X-First"_str, "request"_str).is_ok());
    EXPECT_TRUE(request.header("x-first"_str).unwrap()->to_str().unwrap() == "request"_str);

    auto request_clone = request.try_clone().unwrap();
    ASSERT_TRUE(request_clone.try_set_header("X-First"_str, "clone"_str).is_ok());
    EXPECT_TRUE(request.header("x-first"_str).unwrap()->to_str().unwrap() == "request"_str);
    EXPECT_TRUE(request_clone.header("x-first"_str).unwrap()->to_str().unwrap() == "clone"_str);
}

TEST(http, RstdAsyncPollFuture) {
    auto value = block_on(rstd_wait_yield());
    EXPECT_EQ(value, 42);
}

TEST(http, ErrorModelVariants) {
#if ! defined(LITO_FEAT_QT)
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
    EXPECT_TRUE(rstd::format("{}", curl_error).as_str() == "client request failed"_str);
    EXPECT_TRUE(rstd::format("{}", *client_source).as_str() ==
                as_rstd_str(curl::curl_easy_strerror(curl::CURLcode::CURLE_COULDNT_CONNECT)));

    ncrequest::Error timeout = rstd::into(curl::CURLcode::CURLE_OPERATION_TIMEDOUT);
    EXPECT_TRUE(timeout.is_Timeout());
    EXPECT_EQ(timeout.kind(), ncrequest::ErrorKind::Timeout);
    EXPECT_TRUE(as<rstd::error::Error>(timeout).source().is_none());
    ncrequest::Error multi_timeout =
        rstd::into(ncrequest::CurlMultiError::Easy(curl::CURLcode::CURLE_OPERATION_TIMEDOUT));
    EXPECT_TRUE(multi_timeout.is_Timeout());

    auto multi_error = ncrequest::CurlMultiError::Multi(curl::CURLMcode::CURLM_BAD_HANDLE);
    EXPECT_TRUE(as<rstd::error::Error>(multi_error).source().is_none());
    EXPECT_TRUE(rstd::format("{}", multi_error).as_str() ==
                as_rstd_str(curl::curl_multi_strerror(curl::CURLMcode::CURLM_BAD_HANDLE)));
#else
    ncrequest::Error client = rstd::into(ncrequest::ClientError {
        .backend = ncrequest::ClientBackend::QtNetwork,
        .code    = i32(7),
        .message = String::make("client error"_str),
    });
    EXPECT_EQ(client.kind(), ncrequest::ErrorKind::Client);
    ASSERT_TRUE(client.is_Client());
    EXPECT_EQ(client.as_Client().error.backend, ncrequest::ClientBackend::QtNetwork);
    EXPECT_EQ(client.as_Client().error.code.to_primitive(), 7);
    auto client_source = as<rstd::error::Error>(client).source();
    ASSERT_TRUE(client_source.is_some());
    EXPECT_TRUE(rstd::error::is<ncrequest::ClientError>(*client_source));
    EXPECT_EQ(client_source->as_raw_ptr(), &client.as_Client().error);
    EXPECT_TRUE(rstd::format("{}", client).as_str() == "client request failed"_str);
    EXPECT_TRUE(rstd::format("{}", *client_source).as_str() == "client error"_str);
#endif

    auto             io       = IoError::from_kind(IoErrorKind { IoErrorKind::TimedOut });
    ncrequest::Error io_error = rstd::into(rstd::move(io));
    EXPECT_EQ(io_error.kind(), ncrequest::ErrorKind::Io);
    ASSERT_TRUE(io_error.is_Io());
    EXPECT_EQ(io_error.as_Io().error.kind(), (IoErrorKind { IoErrorKind::TimedOut }));
    auto io_source = as<rstd::error::Error>(io_error).source();
    ASSERT_TRUE(io_source.is_some());
    EXPECT_TRUE(rstd::error::is<IoError>(*io_source));
    EXPECT_EQ(io_source->as_raw_ptr(), &io_error.as_Io().error);
    EXPECT_TRUE(rstd::format("{}", io_error).as_str() == "I/O request failed"_str);
    EXPECT_TRUE(rstd::format("{}", *io_source).as_str() == "timed out"_str);

    auto canceled = ncrequest::Error::Canceled();
    EXPECT_EQ(canceled.kind(), ncrequest::ErrorKind::Canceled);

    auto unsupported = ncrequest::Error::Unsupported("unsupported capability");
    EXPECT_EQ(unsupported.kind(), ncrequest::ErrorKind::Unsupported);
    EXPECT_TRUE(rstd::format("{}", unsupported).as_str() == "unsupported capability"_str);
}

TEST(http, UnifiedOptionsInheritanceAndOverride) {
    auto defaults              = SessionOptions {};
    defaults.timeout.connect   = TimeoutLimit::after(Duration::from_secs(u64(3)));
    defaults.timeout.total     = TimeoutLimit::after(Duration::from_millis(u64(250)));
    defaults.timeout.low_speed = Some(LowSpeedOptions { u64(2), Duration::from_secs(u64(4)) });
    defaults.proxy =
        Proxy::explicit_proxy(lihttpto::Url::parse("socks5://127.0.0.1:1080"_str).unwrap())
            .unwrap();
    defaults.tcp             = Tcp { true, i64(12), i64(6) };
    defaults.tls.verify_peer = false;
    defaults.share           = Share { Some(ncrequest::SessionShare::make().unwrap()) };
    auto request             = make_request("http://localhost/");
    auto inherited = PreparedRequest::prepare(request.try_clone().unwrap(), defaults).unwrap();
    EXPECT_TRUE(inherited.options().timeout().connect.duration().unwrap() ==
                Duration::from_secs(u64(3)));
    EXPECT_EQ(inherited.options().timeout().low_speed->bytes_per_second, u64(2));
    EXPECT_TRUE(inherited.options().timeout().total.duration().unwrap() ==
                Duration::from_millis(u64(250)));
    EXPECT_TRUE(inherited.options().timeout().low_speed->window == Duration::from_secs(u64(4)));
    EXPECT_EQ(inherited.options().proxy().type().unwrap(), Proxy::Type::SOCKS5);
    EXPECT_TRUE(inherited.options().proxy().url().unwrap()->as_ref() ==
                "socks5://127.0.0.1:1080"_str);
    EXPECT_TRUE(inherited.options().tcp().keepalive);
    EXPECT_FALSE(inherited.options().tls().verify_peer);
    EXPECT_TRUE(inherited.options().share().share.is_some());

    auto overrides    = RequestOptions {};
    overrides.timeout = Some(Timeout {});
    overrides.proxy   = Some(Proxy::disabled());
    overrides.tcp     = Some(Tcp {});
    overrides.tls     = Some(SSL {});
    overrides.share   = Some(Share {});
    request.set_options(rstd::move(overrides));
    auto copy      = request.try_clone().unwrap();
    auto effective = PreparedRequest::prepare(rstd::move(copy), defaults).unwrap();
    EXPECT_EQ(effective.options().timeout().total.mode(), TimeoutLimit::Mode::Disabled);
    EXPECT_TRUE(effective.options().timeout().low_speed.is_none());
    EXPECT_EQ(effective.options().proxy().mode(), Proxy::Mode::Disabled);
    EXPECT_FALSE(effective.options().tcp().keepalive);
    EXPECT_TRUE(effective.options().tls().verify_peer);
    EXPECT_TRUE(effective.options().share().share.is_none());
    defaults.proxy = Proxy::system();
    EXPECT_TRUE(inherited.options().proxy().url().unwrap()->as_ref() ==
                "socks5://127.0.0.1:1080"_str);
    EXPECT_FALSE(inherited.options().tls().verify_peer);
    EXPECT_TRUE(request.options().share.is_some());
    EXPECT_TRUE(request.options().share->share.is_none());
}

TEST(http, CookiePersistenceErrors) {
    auto made = ncrequest::SessionShare::make();
    ASSERT_TRUE(made.is_ok());
    auto share   = rstd::move(made).unwrap();
    auto path    = unique_temp_path("cookie-errors");
    auto missing = share.load(path.as_path());
    ASSERT_TRUE(missing.is_err());
    EXPECT_TRUE(missing.unwrap_err().is_Io());
    auto child = path.clone();
    child.push(ref<Path>("missing.txt"_str));
    auto unavailable = share.save(child.as_path());
    ASSERT_TRUE(unavailable.is_err());
    EXPECT_TRUE(unavailable.unwrap_err().is_Io());
    auto directory = share.save(temp_dir().as_path());
    ASSERT_TRUE(directory.is_err());
    EXPECT_TRUE(directory.unwrap_err().is_Io());
    ASSERT_TRUE(share.save(path.as_path()).is_ok());
    auto empty = read_file(path.as_path());
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->starts_with("# Netscape HTTP Cookie File"));
    EXPECT_TRUE(share.load(path.as_path()).is_ok());
    remove_file(path.as_path());
}

#ifndef LITO_FEAT_QT
TEST(http, CookieShareInitializationFailure) {
    auto* fault = std::getenv("NCREQUEST_TEST_CURL_FAILURE");
    if (fault == nullptr || std::string(fault) != "share-init") GTEST_SKIP();
    auto share = ncrequest::SessionShare::make();
    ASSERT_TRUE(share.is_err());
    auto error = rstd::move(share).unwrap_err();
    ASSERT_TRUE(error.is_Client());
    EXPECT_EQ(error.as_Client().error.backend, ncrequest::ClientBackend::CurlShare);
    EXPECT_EQ(error.as_Client().error.code, static_cast<i32>(curl::CURLSHcode::CURLSHE_NOMEM));
    EXPECT_TRUE(ncrequest::SessionShare::make().is_ok());
}
#endif

TEST(http, CookiePersistenceMergeAndCommands) {
    auto        share  = ncrequest::SessionShare::make().unwrap();
    auto        input  = unique_temp_path("cookie-input");
    auto        output = unique_temp_path("cookie-output");
    std::string data   = "# Netscape HTTP Cookie File\r\n";
    for (int i = 0; i < 100; ++i)
        data += "example.com\tFALSE\t/\tFALSE\t0\tkey" + std::to_string(i) + "\toriginal\r\n";
    data += "ALL\r\nSESS\r\nFLUSH\r\nRELOAD\r\nall\r\n";
    data += "Set-Cookie: injected=bad;\tDomain=example.com\r\n";
    data += "#HttpOnly_example.com\tFALSE\t/\tFALSE\t0\thidden\tsecret";
    ASSERT_TRUE(write_file(input.as_path(), data));
    ASSERT_TRUE(share.load(input.as_path()).is_ok());
    auto clone = share.clone();
    ASSERT_TRUE(clone.save(output.as_path()).is_ok());
    auto saved = read_file(output.as_path());
    ASSERT_TRUE(saved.has_value());
    for (int i = 0; i < 100; ++i)
        EXPECT_NE(saved->find("\tkey" + std::to_string(i) + "\toriginal\n"), std::string::npos);
    EXPECT_NE(saved->find("#HttpOnly_example.com"), std::string::npos);
    EXPECT_EQ(saved->find("injected"), std::string::npos);

    data = "example.com\tFALSE\t/\tFALSE\t0\tnew_cookie\tvalue\n";
    data.push_back('\0');
    ASSERT_TRUE(write_file(input.as_path(), data));
    auto invalid = share.load(input.as_path());
    ASSERT_TRUE(invalid.is_err());
    EXPECT_TRUE(invalid.unwrap_err().is_InvalidState());
    ASSERT_TRUE(clone.save(output.as_path()).is_ok());
    auto unchanged = read_file(output.as_path());
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(*unchanged, *saved);

    ASSERT_TRUE(write_file(input.as_path(), "example.com\tFALSE\t/\tFALSE\t0\tkey0\treplaced\n"));
    ASSERT_TRUE(share.load(input.as_path()).is_ok());
    ASSERT_TRUE(clone.save(output.as_path()).is_ok());
    saved = read_file(output.as_path());
    ASSERT_TRUE(saved.has_value());
    EXPECT_NE(saved->find("\tkey0\treplaced\n"), std::string::npos);
    EXPECT_EQ(saved->find("\tkey0\toriginal\n"), std::string::npos);
    remove_file(input.as_path());
    remove_file(output.as_path());
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
#ifdef LITO_FEAT_QT
    EXPECT_EQ(result.timed_out_request.kind, ncrequest::ErrorKind::Unsupported);
#else
    EXPECT_EQ(result.timed_out_request.kind, ncrequest::ErrorKind::Timeout);
#endif

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
        auto req         = make_request(url);
        auto options     = RequestOptions {};
        options.redirect = Some(RedirectOptions::same_origin());
        req.set_options(rstd::move(options));
        return fetch_text_request(rstd::move(session), rstd::move(req));
    });
    ASSERT_TRUE(result.got_response) << result.error;
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.code, 200);
    EXPECT_TRUE(result.has_test_header);
    EXPECT_EQ(result.body, "ncrequest python http server body\n");
}

TEST(http, LocalHttpPreservesRequestHeaderBytes) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();

    auto result = run_http([url = local_http_url(base, "/headers/request-bytes")](auto session) {
        auto request = make_request(url);
        auto headers = lihttpto::Headers {};
        headers
            .add("X-Ncrequest-Bytes"_str,
                 lihttpto::HeaderValue::make("\x80\xc3\xa9\xff"_bytes).unwrap())
            .unwrap();
        request.update_header(headers);
        return fetch_text_request(rstd::move(session), rstd::move(request));
    });
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.body, "80c3a9ff");
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
#if defined(LITO_FEAT_QT)
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
#if ! defined(LITO_FEAT_QT)
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

TEST(http, LocalHttpErrorStatusBody) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();

    for (auto code : { 400, 401, 403, 404, 405, 407, 409, 410, 418, 422, 500, 501, 502, 503 }) {
        SCOPED_TRACE(code);
        auto result = run_http(
            [url = local_http_url(base, "/status?code=" + std::to_string(code))](auto session) {
                return fetch_text(rstd::move(session), url);
            });
        ASSERT_TRUE(result.got_response) << result.error;
        ASSERT_TRUE(result.got_body) << result.error;
        EXPECT_EQ(result.code, code);
        EXPECT_EQ(result.body, "status body\n");
    }
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
#ifdef LITO_FEAT_QT
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Unsupported);
#else
    EXPECT_EQ(result.kind, ncrequest::ErrorKind::Timeout);
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
#ifdef LITO_FEAT_QT
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
#ifdef LITO_FEAT_QT
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
static_assert(! std::is_copy_constructible_v<ncrequest::ResponseBody>);

namespace
{
auto response_before_body(Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto started  = steady_clock::now();
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto elapsed = steady_clock::now() - started;
    auto value   = rstd::move(response).unwrap();
    auto correct = value->head().status.value() == u16(200) &&
                   value->header().contains("x-ncrequest-test"_str) && ! value->is_finished() &&
                   elapsed < milliseconds(700);
    auto bytes   = co_await value->bytes();
    co_return correct&& bytes.is_ok() && string_from_bytes(*bytes) == "delayed body";
}

auto owned_body(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto body = Option<ncrequest::ResponseBody> {};
    {
        auto response = co_await session->get(make_request(url));
        if (response.is_err()) co_return false;
        auto value = rstd::move(response).unwrap();
        body       = Some(value->take_body().unwrap());
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
    auto after_eof      = co_await body->next();
    auto second_collect = co_await body->collect();
    co_return chunks > 1 && contents == download_body() && after_eof.is_err() &&
        after_eof.unwrap_err().is_InvalidState() && second_collect.is_err() &&
        second_collect.unwrap_err().is_InvalidState();
}

auto limited_body(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value = rstd::move(response).unwrap();
    auto bytes = co_await value->bytes(usize(128));
    if (bytes.is_ok() || ! bytes.unwrap_err().is_Protocol()) co_return false;
    auto second = co_await value->text();
    co_return bytes.unwrap_err().as_Protocol().kind ==
            ncrequest::ProtocolError::BodyTooLarge&& second.is_err() &&
        second.unwrap_err().is_InvalidState();
}

struct StreamingSink {
    using Error = ncrequest::Error;
    std::string contents;
    std::size_t writes {};
    bool        fail { false };
    auto        write(Bytes bytes) -> ncrequest::coro<ncrequest::Result<empty>> {
        if (fail) co_return Err(Error::InvalidState("sink rejected chunk"));
        ++writes;
        contents += string_from_bytes(bytes);
        co_return Ok(empty {});
    }
};

auto transfer_body_to_sink(Arc<ncrequest::Session> session, std::string url, bool fail)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto          value = rstd::move(response).unwrap();
    StreamingSink sink;
    sink.fail    = fail;
    auto written = co_await value->read_to_stream(sink);
    if (fail) co_return written.is_err() && written.unwrap_err().is_Sink();
    co_return written.is_ok() && written->to_primitive() == download_body().size() &&
        sink.writes > 1 && sink.contents == download_body();
}

auto body_drop_cancels(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value = rstd::move(response).unwrap();
    {
        auto body = value->take_body();
        if (body.is_err()) co_return false;
    }
    for (int i = 0; i < 50 && ! value->is_finished(); ++i)
        co_await sleep(Duration::from_millis(u64(10)));
    co_return value->is_finished();
}
} // namespace

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
    for (bool fail : { false, true }) {
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
#ifndef LITO_FEAT_QT
    EXPECT_EQ(result.trailer_count, 1u);
#endif
}

namespace
{
auto concurrent_body_read(Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto body  = (*response)->take_body().unwrap();
    auto first = spawn_local(body.next());
    co_await sleep(Duration::from_millis(u64(20)));
    auto second = co_await body.next();
    if (second.is_ok() || ! second.unwrap_err().is_InvalidState()) co_return false;
    first.abort();
    auto aborted     = co_await rstd::move(first);
    auto after_abort = co_await body.next();
    co_return aborted.is_err() && after_abort.is_err() &&
        after_abort.unwrap_err().is_InvalidState();
}

auto close_session(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto pending = spawn_local(session->get(make_request(url)));
    co_await sleep(Duration::from_millis(u64(20)));
    session->close();
    auto joined = co_await rstd::move(pending);
    if (joined.is_err() || joined->is_ok() || ! joined->unwrap_err().is_Canceled()) co_return false;
    auto rejected = co_await session->get(make_request(url));
    co_return rejected.is_err() && rejected.unwrap_err().is_Canceled();
}

auto send_after_session_drop(std::string url) -> ncrequest::coro<bool> {
    auto pending = [url] {
        auto session = ncrequest::Session::make().unwrap();
        return session->get(make_request(url));
    }();
    auto response = co_await rstd::move(pending);
    co_return response.is_err() && response.unwrap_err().is_Canceled();
}
} // namespace

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

#ifndef LITO_FEAT_QT
namespace
{
auto multi_failure(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto first        = spawn_local(session->get(make_request(url)));
    auto second       = spawn_local(session->get(make_request(url)));
    bool native_error = false;
    for (auto* task : { &first, &second }) {
        auto joined = co_await rstd::move(*task);
        if (joined.is_err()) co_return false;
        auto result = rstd::move(*joined);
        auto error  = ncrequest::Error::Canceled();
        if (result.is_ok()) {
            auto body = co_await (*result)->bytes();
            if (body.is_ok()) co_return false;
            error = rstd::move(body).unwrap_err();
        } else {
            error = rstd::move(result).unwrap_err();
        }
        if (error.is_Client()) {
            auto& client = error.as_Client().error;
            if (client.backend != ncrequest::ClientBackend::CurlMulti ||
                client.code != static_cast<i32>(curl::CURLMcode::CURLM_INTERNAL_ERROR))
                co_return false;
            native_error = true;
        } else if (! error.is_Canceled())
            co_return false;
    }
    auto                     rejected = co_await session->get(make_request(url));
    co_return native_error&& rejected.is_err() && rejected.unwrap_err().is_Canceled();
}

auto close_many(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    using Pending = rstd::async::JoinHandle<ncrequest::Result<Arc<ncrequest::Response>>>;
    auto pending  = Vec<Pending>::make();
    for (int i = 0; i < 16; ++i) pending.push(spawn_local(session->get(make_request(url))));
    co_await sleep(Duration::from_millis(u64(10)));
    session->close();
    for (auto& task : pending) {
        auto joined = co_await rstd::move(task);
        if (joined.is_err() || joined->is_ok() || ! joined->unwrap_err().is_Canceled())
            co_return false;
    }
    co_return true;
}
} // namespace

TEST(http, LocalHttpCurlMultiFailure) {
    auto  base    = local_http_base_url();
    auto* failure = std::getenv("NCREQUEST_TEST_CURL_FAILURE");
    if (base.empty() || failure == nullptr) GTEST_SKIP();
    auto path = std::string(failure).starts_with("remove") ? "/empty" : "/delay";
    EXPECT_TRUE(run_http([url = local_http_url(base, path)](auto session) {
        return multi_failure(rstd::move(session), url);
    }));
}

TEST(http, LocalHttpCloseQueuedRequests) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/delay")](auto session) {
        return close_many(rstd::move(session), url);
    }));
}
#endif

namespace
{
template<class T>
concept NativeSessionAccess = requires(T& value) { value.channel(); };
template<class T>
concept NativeResponseAccess = requires(T& value) { value.pause_recv(true); };
static_assert(! NativeSessionAccess<ncrequest::Session>);
static_assert(! NativeResponseAccess<ncrequest::Response>);

auto truncated_body(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto        body = (*response)->take_body().unwrap();
    std::string partial;
    for (;;) {
        auto next = co_await body.next();
        if (next.is_err()) co_return partial == "partial" && next.unwrap_err().is_Client();
        if (next->is_none()) co_return false;
        partial += string_from_bytes(**next);
    }
}

auto zero_limit_empty_body(Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto empty = co_await (*response)->bytes(usize());
    co_return empty.is_ok() && empty->size() == usize();
}
} // namespace

TEST(http, LocalHttpBodyErrorIsNotEof) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    for (auto code : { 200, 404, 500 }) {
        SCOPED_TRACE(code);
        EXPECT_TRUE(
            run_http([url = local_http_url(base, "/truncated-body?code=" + std::to_string(code))](
                         auto session) {
                return truncated_body(rstd::move(session), url);
            }));
    }
    EXPECT_TRUE(run_http([url = local_http_url(base, "/empty")](auto session) {
        return zero_limit_empty_body(rstd::move(session), url);
    }));
}

namespace
{
auto send_method(Arc<ncrequest::Session> session, std::string url, std::string method,
                 int body_kind) -> ncrequest::coro<bool> {
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

auto reject_invalid_request(Arc<ncrequest::Session> session) -> ncrequest::coro<bool> {
    auto missing_url = co_await session->send(ncrequest::Request {});
    if (missing_url.is_ok() || ! missing_url.unwrap_err().is_InvalidState()) co_return false;
    auto head = make_request("http://127.0.0.1:1/");
    head.try_set_method("HEAD"_str).unwrap();
    head.set_body(bytes_from_string("body"));
    auto response = co_await session->send(rstd::move(head));
    co_return response.is_err() && response.unwrap_err().is_InvalidState();
}

auto abort_owned_upload(Arc<ncrequest::Session> session, std::string base)
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
} // namespace

TEST(http, RequestMethodAndOwnedBody) {
    auto request = make_request("http://127.0.0.1/");
    EXPECT_TRUE(request.method().as_ref() == "GET"_str);
    ASSERT_TRUE(request.try_set_method("REPORT"_str).is_ok());
    for (auto invalid : { "", "GET /", "POST\r\nX: injected" }) {
        EXPECT_TRUE(request.try_set_method(as_rstd_str(invalid)).is_err());
        EXPECT_TRUE(request.method().as_ref() == "REPORT"_str);
    }
    request.set_method(lihttpto::Method::parse("PATCH"_str).unwrap());
    request.set_body(bytes_from_string("owned"));
    auto cloned = request.try_clone().unwrap();
    request.clear_body();
    EXPECT_TRUE(request.body().bytes().is_none());
    ASSERT_TRUE(cloned.body().bytes().is_some());
    EXPECT_TRUE(cloned.method().as_ref() == "PATCH"_str);
    EXPECT_EQ(string_from_bytes(Bytes::copy_from_slice(cloned.body().bytes()->as_slice())),
              "owned");
    EXPECT_TRUE(cloned.validate().is_ok());
    auto reader = BodyReader { [](byte*, usize) {
                                  return usize();
                              },
                               Some(usize()) };
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
    for (auto method : { "GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "REPORT", "HEAD" }) {
        for (int body_kind = 0; body_kind != (std::string_view(method) == "HEAD" ? 2 : 3);
             ++body_kind) {
            SCOPED_TRACE(std::string(method) + " body " + std::to_string(body_kind));
            EXPECT_TRUE(
                run_http([url = local_http_url(base, "/method"), method, body_kind](auto session) {
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

TEST(http, RequestRejectsNonHttpUrlRecords) {
    for (auto input : { "file:///tmp/not-opened"_str,
                        "blob:https://example.com/id"_str,
                        "ftp://example.com/"_str,
                        "ws://example.com/"_str,
                        "data:hello"_str }) {
        auto url     = lihttpto::Url::parse(input).unwrap();
        auto request = ncrequest::Request(rstd::move(url));
        EXPECT_TRUE(request.validate().is_err());
        EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), SessionOptions {}).is_err());
    }
    auto valid = ncrequest::Request(lihttpto::Url::parse("HTTPS://EXAMPLE.com/"_str).unwrap());
    EXPECT_TRUE(valid.validate().is_ok());
}

TEST(http, LocalHttpAbortOwnedUpload) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([base](auto session) {
        return abort_owned_upload(rstd::move(session), base);
    }));
}

namespace
{
auto socket_endpoint(std::string_view path) -> Endpoint {
    return Endpoint::unix_socket(PathBuf::from(as_rstd_str(path))).unwrap();
}

auto socket_path(const Endpoint& endpoint) -> ref<str> {
    auto path = endpoint.socket_path().unwrap();
    return path.to_str().unwrap();
}

auto endpoint_request(std::string url, Endpoint endpoint) -> ncrequest::Request {
    auto request     = make_request(url);
    auto options     = RequestOptions {};
    options.endpoint = Some(rstd::move(endpoint));
    request.set_options(rstd::move(options));
    return request;
}

#if ! defined(LITO_FEAT_QT)
auto local_socket_path() -> std::string {
    auto* path = std::getenv("NCREQUEST_TEST_UNIX_SOCKET");
    return path == nullptr ? std::string {} : std::string(path);
}
#endif
} // namespace

TEST(http, EndpointOptionsInheritanceAndSnapshot) {
    auto defaults     = SessionOptions {};
    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    auto request      = make_request("http://podman.invalid/v6/libpod/info?detail=1");
    auto inherited    = PreparedRequest::prepare(request.try_clone().unwrap(), defaults).unwrap();
    EXPECT_TRUE(socket_path(inherited.options().endpoint()) == "/tmp/session.sock"_str);
    EXPECT_TRUE(inherited.request().url() == "http://podman.invalid/v6/libpod/info?detail=1"_str);

    auto override     = RequestOptions {};
    override.endpoint = Some(socket_endpoint("/tmp/request.sock"));
    request.set_options(rstd::move(override));
    auto cloned       = request.try_clone().unwrap();
    auto selected     = PreparedRequest::prepare(request.try_clone().unwrap(), defaults).unwrap();
    defaults.endpoint = Endpoint::network();
    request.set_options(RequestOptions {});
    EXPECT_TRUE(socket_path(selected.options().endpoint()) == "/tmp/request.sock"_str);
    EXPECT_TRUE(socket_path(*cloned.options().endpoint) == "/tmp/request.sock"_str);
    EXPECT_TRUE(socket_path(inherited.options().endpoint()) == "/tmp/session.sock"_str);

    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    auto network      = endpoint_request("http://localhost/", Endpoint::network());
    auto reset        = PreparedRequest::prepare(rstd::move(network), defaults).unwrap();
    EXPECT_TRUE(reset.options().endpoint().socket_path().is_none());
}

TEST(http, EndpointInvalidPathsAndConflicts) {
    EXPECT_TRUE(Endpoint::unix_socket(PathBuf {}).is_err());
    EXPECT_TRUE(Endpoint::unix_socket(PathBuf::from("/tmp/one\0two"_str)).is_err());
    auto defaults     = SessionOptions {};
    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    auto request      = make_request("http://localhost/");
    auto overrides    = RequestOptions {};
    overrides.proxy   = Some(
        Proxy::explicit_proxy(lihttpto::Url::parse("http://127.0.0.1:1"_str).unwrap()).unwrap());
    request.set_options(overrides.clone());
    auto proxy = PreparedRequest::prepare(request.try_clone().unwrap(), defaults);
    ASSERT_TRUE(proxy.is_err());
    EXPECT_TRUE(proxy.unwrap_err().is_InvalidState());
    overrides.proxy = Some(Proxy::disabled());
    overrides.tcp   = Some(Tcp { true, i64(120), i64(60) });
    request.set_options(overrides.clone());
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_err());
    overrides.tcp = Some(Tcp {});
    request.set_options(rstd::move(overrides));
    EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), defaults).is_ok());
}

TEST(http, LocalHttpUnixEndpoint) {
#if ! defined(LITO_FEAT_QT)
    auto path = local_socket_path();
    if (path.empty()) GTEST_SKIP();
    auto defaults     = SessionOptions {};
    defaults.endpoint = socket_endpoint(path);
    auto session      = ncrequest::Session::make(rstd::move(defaults)).unwrap();
    auto result       = block_on(fetch_text_request(
        session.clone(), make_request("http://podman.invalid:8087/endpoint?detail=1")));
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.body, "unix\npodman.invalid:8087\n/endpoint?detail=1\n");

    auto network_url = local_http_url(local_http_base_url(), "/endpoint?network=1");
    auto reset       = block_on(
        fetch_text_request(session.clone(), endpoint_request(network_url, Endpoint::network())));
    ASSERT_TRUE(reset.got_body) << reset.error;
    EXPECT_EQ(reset.body.substr(0, 4), "tcp\n");

    auto failed = block_on(fetch_text_request(
        session.clone(),
        endpoint_request("http://podman.invalid/text", socket_endpoint(path + ".missing"))));
    EXPECT_TRUE(failed.got_error);
    EXPECT_EQ(failed.error_kind, ncrequest::ErrorKind::Client);

    auto recovered = block_on(
        fetch_text_request(rstd::move(session), make_request("http://podman.invalid/text")));
    ASSERT_TRUE(recovered.got_body) << recovered.error;
#else
    GTEST_SKIP() << "curl-only Unix socket runtime test";
#endif
}

TEST(http, LocalHttpUnixRequestOverride) {
#if ! defined(LITO_FEAT_QT)
    auto path = local_socket_path();
    if (path.empty()) GTEST_SKIP();
    auto result = run_http([path](auto session) {
        return fetch_text_request(
            rstd::move(session),
            endpoint_request("http://not-resolved.invalid/endpoint", socket_endpoint(path)));
    });
    ASSERT_TRUE(result.got_body) << result.error;
    EXPECT_EQ(result.body, "unix\nnot-resolved.invalid\n/endpoint\n");
#else
    GTEST_SKIP() << "curl-only Unix socket runtime test";
#endif
}

TEST(http, UnifiedOptionsValidationAndInheritedConflicts) {
    auto defaults     = SessionOptions {};
    defaults.endpoint = socket_endpoint("/tmp/session.sock");
    defaults.proxy =
        Proxy::explicit_proxy(lihttpto::Url::parse("http://127.0.0.1:1"_str).unwrap()).unwrap();
    auto request = make_request("http://localhost/");
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_err());
    auto options  = RequestOptions {};
    options.proxy = Some(Proxy::disabled());
    request.set_options(options.clone());
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_ok());
    auto invalid_timeout  = Timeout {};
    invalid_timeout.total = TimeoutLimit::after(Duration {});
    options.timeout       = Some(invalid_timeout);
    request.set_options(options.clone());
    EXPECT_TRUE(PreparedRequest::prepare(request.try_clone().unwrap(), defaults).is_err());
    options.timeout = None();
    options.tcp     = Some(Tcp { false, i64(-1), i64(0) });
    request.set_options(rstd::move(options));
    EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), defaults).is_err());
}

TEST(http, RequestBodyReaderOwnership) {
    EXPECT_TRUE(RequestBody::from_reader(BodyReader {}).is_err());
    auto reader = BodyReader { [](byte*, usize) {
                                  return usize();
                              },
                               Some(usize()) };
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

namespace
{
#if ! defined(LITO_FEAT_QT)
auto reader_upload(Arc<ncrequest::Session> session, std::string url, bool known_empty)
    -> ncrequest::coro<bool> {
    std::size_t calls  = 0;
    auto        reader = BodyReader {};
    if (known_empty) reader.size = Some(usize());
    reader.callback = [&calls, position = Box<usize>::make()](byte* buffer,
                                                              usize capacity) mutable {
        ++calls;
        constexpr auto payload = "callback payload"_str;
        auto           count   = rstd::min(payload.size() - *position, capacity);
        rstd::mem::memcpy(buffer, payload.data() + position->to_primitive(), count);
        *position += count;
        return count;
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
} // namespace

TEST(http, LocalHttpReaderLength) {
#if ! defined(LITO_FEAT_QT)
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    for (bool empty : { false, true }) {
        EXPECT_TRUE(run_http([url = local_http_url(base, "/body-reader"), empty](auto session) {
            return reader_upload(rstd::move(session), url, empty);
        }));
    }
#else
    GTEST_SKIP() << "Qt Network does not support body readers";
#endif
}

TEST(http, RequestHeaderPreservesAbsentAndEmpty) {
    auto request = ncrequest::Request {};
    EXPECT_TRUE(request.header("x-value"_str).is_none());
    ASSERT_TRUE(request.try_set_header("x-value"_str, ""_str).is_ok());
    auto value = request.header("x-value"_str);
    ASSERT_TRUE(value.is_some());
    EXPECT_TRUE((*value)->as_slice().is_empty());
}

namespace
{
auto reject_invalid_text(Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto text = co_await (*response)->text();
    co_return text.is_err() && text.unwrap_err().is_Protocol() &&
        text.unwrap_err().as_Protocol().kind == ncrequest::ProtocolError::InvalidUtf8;
}
} // namespace

TEST(http, LocalHttpTextRejectsInvalidUtf8) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(run_http([url = local_http_url(base, "/download.bin")](auto session) {
        return reject_invalid_text(rstd::move(session), url);
    }));
}

TEST(http, ProxyModesAndValidation) {
    EXPECT_EQ(Proxy {}.mode(), Proxy::Mode::System);
    EXPECT_TRUE(Proxy::system().url().is_none());
    EXPECT_TRUE(Proxy::disabled().type().is_none());
    EXPECT_TRUE(Proxy::disabled().port().is_none());
    for (auto text : { "relative", "http://", "http://host:65536" })
        EXPECT_TRUE(lihttpto::Url::parse(as_rstd_str(text)).is_err());
    for (auto text : { "http://host:0",
                       "http://host/path",
                       "http://host/?query",
                       "http://host/#fragment",
                       "ftp://host",
                       "http://secret:password@host" }) {
        auto parsed = lihttpto::Url::parse(as_rstd_str(text));
        ASSERT_TRUE(parsed.is_ok());
        auto value = Proxy::explicit_proxy(rstd::move(parsed).unwrap());
        EXPECT_TRUE(value.is_err()) << text;
        if (value.is_err()) {
            auto message = rstd::format("{}", value.unwrap_err());
            EXPECT_FALSE(message.as_str().contains("secret"_str));
            EXPECT_FALSE(message.as_str().contains("password"_str));
        }
    }
    auto original =
        Proxy::explicit_proxy(lihttpto::Url::parse("HtTp://[::1]:3128/"_str).unwrap()).unwrap();
    auto copy = original.clone();
    original  = Proxy::disabled();
    EXPECT_EQ(copy.type().unwrap(), Proxy::Type::HTTP);
    EXPECT_EQ(copy.port().unwrap(), u16(3128));
    EXPECT_TRUE(copy.url().unwrap()->host().unwrap() == "[::1]"_str);
    for (auto text : { "http://host", "http://host:", "http://host:80" }) {
        auto value =
            Proxy::explicit_proxy(lihttpto::Url::parse(as_rstd_str(text)).unwrap()).unwrap();
        EXPECT_EQ(value.port().unwrap(), u16(80));
    }
    for (auto text : { "https://host", "https://host:", "https://host:443" }) {
        auto value =
            Proxy::explicit_proxy(lihttpto::Url::parse(as_rstd_str(text)).unwrap()).unwrap();
        EXPECT_EQ(value.port().unwrap(), u16(443));
    }
    for (auto text : { "socks4://host", "socks4a://host", "socks5://host", "socks5h://host" }) {
        auto value =
            Proxy::explicit_proxy(lihttpto::Url::parse(as_rstd_str(text)).unwrap()).unwrap();
        EXPECT_EQ(value.mode(), Proxy::Mode::Explicit);
        EXPECT_EQ(value.port().unwrap(), u16(1080));
    }
    auto session     = SessionOptions {};
    session.endpoint = Endpoint::unix_socket(PathBuf::from("/tmp/proxy-test.sock"_str)).unwrap();
    auto effective   = EffectiveOptions::resolve(session, RequestOptions {}).unwrap();
    EXPECT_EQ(effective.proxy().mode(), Proxy::Mode::Disabled);
}

TEST(http, LocalHttpProxyPolicies) {
#if ! defined(LITO_FEAT_QT)
    auto* proxy_address = std::getenv("NCREQUEST_TEST_PROXY_URL");
    if (proxy_address == nullptr) GTEST_SKIP();
    auto session = ncrequest::Session::make().unwrap();
    auto base    = local_http_base_url();
    auto system  = block_on(fetch_text(session.clone(), local_http_url(base, "/endpoint")));
    ASSERT_TRUE(system.got_body) << system.error;
    EXPECT_EQ(system.body, "proxy\n" + local_http_url(base, "/endpoint") + "\n");

    auto direct   = make_request(local_http_url(base, "/endpoint"));
    auto options  = RequestOptions {};
    options.proxy = Some(Proxy::disabled());
    direct.set_options(rstd::move(options));
    auto disabled = block_on(fetch_text_request(session.clone(), rstd::move(direct)));
    ASSERT_TRUE(disabled.got_body) << disabled.error;
    EXPECT_EQ(disabled.body.substr(0, 4), "tcp\n");

    auto bypass_url = base;
    bypass_url.replace(bypass_url.find("127.0.0.1"), 9, "localhost");
    auto bypass = block_on(fetch_text(session.clone(), local_http_url(bypass_url, "/endpoint")));
    ASSERT_TRUE(bypass.got_body) << bypass.error;
    EXPECT_EQ(bypass.body.substr(0, 4), "tcp\n");

    auto explicit_request  = make_request("http://explicit-bypass.invalid/check?wire=1");
    auto explicit_options  = RequestOptions {};
    explicit_options.proxy = Some(
        Proxy::explicit_proxy(lihttpto::Url::parse(as_rstd_str(proxy_address)).unwrap()).unwrap());
    explicit_request.set_options(rstd::move(explicit_options));
    auto explicit_result =
        block_on(fetch_text_request(session.clone(), rstd::move(explicit_request)));
    ASSERT_TRUE(explicit_result.got_body) << explicit_result.error;
    EXPECT_EQ(explicit_result.body, "proxy\nhttp://explicit-bypass.invalid/check?wire=1\n");

    auto inherited = block_on(fetch_text(rstd::move(session), local_http_url(base, "/endpoint")));
    ASSERT_TRUE(inherited.got_body) << inherited.error;
    EXPECT_EQ(inherited.body, system.body);
#else
    GTEST_SKIP() << "curl proxy runtime test";
#endif
}

TEST(http, TimeoutValidationAndOverride) {
    auto defaults = SessionOptions {};
    EXPECT_EQ(defaults.timeout.connect.mode(), TimeoutLimit::Mode::BackendDefault);
    EXPECT_EQ(defaults.timeout.total.mode(), TimeoutLimit::Mode::Disabled);
    EXPECT_TRUE(defaults.timeout.low_speed.is_none());
    defaults.timeout.total = TimeoutLimit::after(Duration::from_secs(u64(2)));
    auto options           = RequestOptions {};
    auto inherited         = EffectiveOptions::resolve(defaults, options).unwrap();
    EXPECT_TRUE(inherited.timeout().total.duration().unwrap() == Duration::from_secs(u64(2)));
    options.timeout = Some(Timeout {});
    auto reset      = EffectiveOptions::resolve(defaults, options).unwrap();
    EXPECT_EQ(reset.timeout().total.mode(), TimeoutLimit::Mode::Disabled);
    EXPECT_TRUE(inherited.timeout().total.duration().is_some());
    auto value    = Timeout {};
    value.connect = TimeoutLimit::disabled();
    EXPECT_TRUE(value.validate().is_ok());
    value.connect = TimeoutLimit::after(Duration {});
    EXPECT_TRUE(value.validate().is_err());
    value.connect = TimeoutLimit::backend_default();
    value.total   = TimeoutLimit::after(Duration {});
    EXPECT_TRUE(value.validate().is_err());
    value.total = TimeoutLimit::after(Duration::from_nanos(u64(1)));
    EXPECT_TRUE(value.validate().is_ok());
    value.low_speed = Some(LowSpeedOptions { u64(), Duration::from_secs(u64(1)) });
    EXPECT_TRUE(value.validate().is_err());
    value.low_speed = Some(LowSpeedOptions { u64(1), Duration {} });
    EXPECT_TRUE(value.validate().is_err());
    value.low_speed = Some(LowSpeedOptions { u64(1), Duration::from_millis(u64(1)) });
    EXPECT_TRUE(value.validate().is_ok());
}

TEST(http, LocalHttpTimeoutPolicies) {
#if ! defined(LITO_FEAT_QT)
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto session = ncrequest::Session::make().unwrap();
    auto fetch   = [&](std::string url, Timeout timeout) {
        auto request    = make_request(url);
        auto options    = RequestOptions {};
        options.proxy   = Some(Proxy::disabled());
        options.timeout = Some(timeout);
        request.set_options(rstd::move(options));
        return block_on(fetch_text_request(session.clone(), rstd::move(request)));
    };
    auto policy         = Timeout {};
    policy.total        = TimeoutLimit::after(Duration::from_millis(u64(100)));
    auto before_headers = fetch(local_http_url(base, "/slow-first-byte"), policy);
    EXPECT_FALSE(before_headers.got_response);
    EXPECT_EQ(before_headers.error_kind, ncrequest::ErrorKind::Timeout) << before_headers.error;
    auto active = fetch(local_http_url(base, "/timeout-trickle"), policy);
    EXPECT_TRUE(active.got_response);
    EXPECT_FALSE(active.got_body);
    EXPECT_EQ(active.error_kind, ncrequest::ErrorKind::Timeout) << active.error;

    policy.total   = TimeoutLimit::disabled();
    policy.connect = TimeoutLimit::after(Duration::from_millis(u64(100)));
    auto connected = fetch(local_http_url(base, "/delay"), policy);
    ASSERT_TRUE(connected.got_body) << connected.error;
    EXPECT_EQ(connected.body, "delayed\n");
    policy.low_speed = Some(LowSpeedOptions { u64(1000), Duration::from_millis(u64(1)) });
    auto slow        = fetch(local_http_url(base, "/timeout-stall"), policy);
    EXPECT_TRUE(slow.got_response);
    EXPECT_FALSE(slow.got_body);
    EXPECT_EQ(slow.error_kind, ncrequest::ErrorKind::Timeout) << slow.error;

    policy.low_speed = None();
    auto restored    = fetch(local_http_url(base, "/delayed-body"), policy);
    ASSERT_TRUE(restored.got_body) << restored.error;
    EXPECT_EQ(restored.body, "delayed body");

    policy.total = TimeoutLimit::after(Duration::from_nanos(u64(1)));
    auto tiny    = fetch(local_http_url(base, "/delay"), policy);
    EXPECT_FALSE(tiny.got_body);
    EXPECT_EQ(tiny.error_kind, ncrequest::ErrorKind::Timeout) << tiny.error;
    policy.total     = TimeoutLimit::disabled();
    policy.connect   = TimeoutLimit::disabled();
    auto unsupported = fetch(local_http_url(base, "/text"), policy);
    EXPECT_FALSE(unsupported.got_response);
    EXPECT_EQ(unsupported.error_kind, ncrequest::ErrorKind::Unsupported);
    policy.connect = TimeoutLimit::backend_default();
    policy.total   = TimeoutLimit::after(Duration::from_secs(u64::MAX));
    auto overflow  = fetch(local_http_url(base, "/text"), policy);
    EXPECT_FALSE(overflow.got_response);
    EXPECT_EQ(overflow.error_kind, ncrequest::ErrorKind::InvalidState);
    policy.total        = TimeoutLimit::disabled();
    policy.low_speed    = Some(LowSpeedOptions { u64::MAX, Duration::from_secs(u64(1)) });
    auto speed_overflow = fetch(local_http_url(base, "/text"), policy);
    EXPECT_EQ(speed_overflow.error_kind, ncrequest::ErrorKind::InvalidState);
    policy.low_speed     = Some(LowSpeedOptions { u64(1), Duration::from_secs(u64::MAX) });
    auto window_overflow = fetch(local_http_url(base, "/text"), policy);
    EXPECT_EQ(window_overflow.error_kind, ncrequest::ErrorKind::InvalidState);
#else
    GTEST_SKIP() << "curl timeout runtime test";
#endif
}

TEST(http, LocalHttpConnectTimeout) {
#if ! defined(LITO_FEAT_QT)
    auto* url = std::getenv("NCREQUEST_TEST_TLS_STALL_URL");
    if (url == nullptr) GTEST_SKIP();
    auto request    = make_request(url);
    auto options    = RequestOptions {};
    auto timeout    = Timeout {};
    timeout.connect = TimeoutLimit::after(Duration::from_millis(u64(100)));
    timeout.total   = TimeoutLimit::after(Duration::from_secs(u64(2)));
    options.timeout = Some(timeout);
    options.proxy   = Some(Proxy::disabled());
    request.set_options(rstd::move(options));
    auto started = steady_clock::now();
    auto result =
        block_on(fetch_text_request(ncrequest::Session::make().unwrap(), rstd::move(request)));
    auto elapsed = std::chrono::duration_cast<milliseconds>(steady_clock::now() - started);
    EXPECT_FALSE(result.got_response);
    EXPECT_EQ(result.error_kind, ncrequest::ErrorKind::Timeout) << result.error;
    EXPECT_LT(elapsed.count(), 1500);
#else
    GTEST_SKIP() << "curl connect timeout runtime test";
#endif
}

TEST(http, TlsOptionsValidationAndSnapshot) {
    auto defaults = SessionOptions {};
    EXPECT_TRUE(defaults.tls.verify_peer);
    EXPECT_TRUE(defaults.tls.verify_hostname);
    defaults.tls.ca_bundle       = Some(PathBuf::from("/tmp/root.pem"_str));
    defaults.tls.client_identity = Some(TlsClientIdentity {
        PathBuf::from("/tmp/client.pem"_str), PathBuf::from("/tmp/client-key.pem"_str) });
    auto effective               = EffectiveOptions::resolve(defaults, RequestOptions {}).unwrap();
    defaults.tls                 = SSL {};
    EXPECT_TRUE(effective.tls().ca_bundle->as_path() ==
                PathBuf::from("/tmp/root.pem"_str).as_path());
    EXPECT_TRUE(effective.tls().client_identity.is_some());
    EXPECT_FALSE(effective.redirect().enabled());
    defaults.tls = effective.tls().clone();
    auto options = RequestOptions {};
    options.tls  = Some(SSL {});
    auto reset   = EffectiveOptions::resolve(defaults, options).unwrap();
    EXPECT_TRUE(reset.tls().ca_bundle.is_none());
    EXPECT_TRUE(reset.tls().client_identity.is_none());
    EXPECT_FALSE(reset.redirect().enabled());
    auto invalid      = SSL {};
    invalid.ca_bundle = Some(PathBuf {});
    EXPECT_TRUE(invalid.validate().is_err());
    invalid.ca_bundle = Some(PathBuf::from("secret\0tail"_str));
    auto error        = invalid.validate().unwrap_err();
    EXPECT_FALSE(rstd::format("{}", error).as_str().contains("secret"_str));
    invalid.ca_bundle = None();
    invalid.client_identity =
        Some(TlsClientIdentity { PathBuf::from("/tmp/cert"_str), PathBuf {} });
    EXPECT_TRUE(invalid.validate().is_err());
    auto identity = effective.tls().clone();
    auto request  = make_request("http://localhost/");
    options.tls   = Some(identity.clone());
    request.set_options(options.clone());
    EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), defaults).is_err());
    request = make_request("https://localhost/");
    request.set_options(rstd::move(options));
    EXPECT_TRUE(PreparedRequest::prepare(rstd::move(request), defaults).is_ok());
}

TEST(http, LocalHttpsVerificationAndIdentity) {
#if ! defined(LITO_FEAT_QT)
    auto* base = std::getenv("NCREQUEST_TEST_HTTPS_URL");
    if (base == nullptr) GTEST_SKIP();
    auto path = [](const char* name) {
        return PathBuf::from(as_rstd_str(std::getenv(name)));
    };
    auto session = ncrequest::Session::make().unwrap();
    auto fetch   = [&](std::string url, SSL tls) {
        auto request    = make_request(url);
        auto options    = RequestOptions {};
        options.proxy   = Some(Proxy::disabled());
        auto timeout    = Timeout {};
        timeout.total   = TimeoutLimit::after(Duration::from_secs(u64(3)));
        options.timeout = Some(timeout);
        options.tls     = Some(rstd::move(tls));
        request.set_options(rstd::move(options));
        return block_on(fetch_text_request(session.clone(), rstd::move(request)));
    };
    auto url       = std::string(base) + "/text";
    auto untrusted = fetch(url, SSL {});
    EXPECT_EQ(untrusted.client_code,
              static_cast<int>(curl::CURLcode::CURLE_PEER_FAILED_VERIFICATION));
    auto tls      = SSL {};
    tls.ca_bundle = Some(path("NCREQUEST_TEST_TLS_CA"));
    auto trusted  = fetch(url, tls.clone());
    ASSERT_TRUE(trusted.got_body) << trusted.error;

    auto mismatch_url = url;
    mismatch_url.replace(mismatch_url.find("localhost"), 9, "127.0.0.1");
    auto mismatch = fetch(mismatch_url, tls.clone());
    EXPECT_EQ(mismatch.client_code,
              static_cast<int>(curl::CURLcode::CURLE_PEER_FAILED_VERIFICATION));
    tls.verify_hostname = false;
    EXPECT_TRUE(fetch(mismatch_url, tls.clone()).got_body);
    tls.ca_bundle = Some(path("NCREQUEST_TEST_TLS_CLIENT_CERT"));
    auto wrong_ca = fetch(mismatch_url, tls.clone());
    EXPECT_EQ(wrong_ca.client_code,
              static_cast<int>(curl::CURLcode::CURLE_PEER_FAILED_VERIFICATION));
    tls.ca_bundle       = None();
    tls.verify_peer     = false;
    tls.verify_hostname = true;
    EXPECT_TRUE(fetch(url, tls.clone()).got_body);
    auto still_mismatch = fetch(mismatch_url, tls.clone());
    EXPECT_EQ(still_mismatch.client_code,
              static_cast<int>(curl::CURLcode::CURLE_PEER_FAILED_VERIFICATION));
    tls.verify_hostname = false;
    EXPECT_TRUE(fetch(mismatch_url, tls.clone()).got_body);
    EXPECT_EQ(fetch(url, SSL {}).client_code,
              static_cast<int>(curl::CURLcode::CURLE_PEER_FAILED_VERIFICATION));

    tls              = SSL {};
    tls.ca_bundle    = Some(path("NCREQUEST_TEST_TLS_CA"));
    auto mtls_url    = std::string(std::getenv("NCREQUEST_TEST_MTLS_URL"));
    auto no_identity = fetch(mtls_url + "/text", tls.clone());
    EXPECT_TRUE(no_identity.got_error);
    EXPECT_FALSE(no_identity.got_response);
    tls.client_identity = Some(TlsClientIdentity { path("NCREQUEST_TEST_TLS_CLIENT_CERT"),
                                                   path("NCREQUEST_TEST_TLS_CLIENT_KEY") });
    auto authenticated  = fetch(mtls_url + "/text", tls.clone());
    ASSERT_TRUE(authenticated.got_body) << authenticated.error;
    auto redirect = fetch(mtls_url + "/redirect", tls.clone());
    EXPECT_EQ(redirect.code, 302) << redirect.error << " code=" << redirect.client_code;
    tls.client_identity->private_key = path("NCREQUEST_TEST_TLS_WRONG_KEY");
    auto bad_key                     = fetch(mtls_url + "/text", tls.clone());
    EXPECT_TRUE(bad_key.got_error);
    EXPECT_FALSE(bad_key.got_response);
    EXPECT_TRUE(bad_key.client_code == static_cast<int>(curl::CURLcode::CURLE_SSL_CERTPROBLEM) ||
                bad_key.client_code ==
                    static_cast<int>(curl::CURLcode::CURLE_BAD_FUNCTION_ARGUMENT))
        << bad_key.error << " code=" << bad_key.client_code;
    tls.client_identity = None();
    EXPECT_TRUE(fetch(mtls_url + "/text", tls.clone()).got_error);
    tls.ca_bundle = Some(PathBuf::from("/no-such-ncrequest-secret-ca"_str));
    auto missing  = fetch(url, tls.clone());
    EXPECT_TRUE(missing.got_error);
    EXPECT_EQ(missing.error.find("secret"), std::string::npos);

    auto proxy_request = make_request("http://unused.invalid/text");
    auto proxy_options = RequestOptions {};
    proxy_options.proxy =
        Some(Proxy::explicit_proxy(lihttpto::Url::parse(as_rstd_str(base)).unwrap()).unwrap());
    tls.ca_bundle       = Some(path("NCREQUEST_TEST_TLS_CA"));
    tls.verify_peer     = false;
    tls.verify_hostname = false;
    proxy_options.tls   = Some(rstd::move(tls));
    proxy_request.set_options(rstd::move(proxy_options));
    auto proxy_result = block_on(fetch_text_request(session.clone(), rstd::move(proxy_request)));
    EXPECT_EQ(proxy_result.client_code,
              static_cast<int>(curl::CURLcode::CURLE_PEER_FAILED_VERIFICATION));
#else
    GTEST_SKIP() << "curl TLS runtime test";
#endif
}

TEST(http, RedirectOriginsAndOptions) {
    auto base = lihttpto::Url::parse("https://EXAMPLE.com/a"_str).unwrap();
    EXPECT_TRUE(
        base.same_http_origin(lihttpto::Url::parse("https://example.com:443/b"_str).unwrap()));
    EXPECT_FALSE(base.same_http_origin(lihttpto::Url::parse("http://example.com/b"_str).unwrap()));
    EXPECT_FALSE(
        base.same_http_origin(lihttpto::Url::parse("https://example.com:444/b"_str).unwrap()));
    EXPECT_FALSE(
        base.same_http_origin(lihttpto::Url::parse("https://other.invalid/b"_str).unwrap()));
    EXPECT_FALSE(base.same_http_origin(lihttpto::Url::parse("ftp://example.com/b"_str).unwrap()));
    EXPECT_TRUE(lihttpto::Url::parse("https://example.com:65536/b"_str).is_err());
    auto defaults = SessionOptions {};
    EXPECT_FALSE(defaults.redirect.enabled());
    defaults.redirect = RedirectOptions::same_origin(u32(2));
    auto options      = RequestOptions {};
    auto inherited    = EffectiveOptions::resolve(defaults, options).unwrap();
    EXPECT_EQ(inherited.redirect().max_hops(), u32(2));
    options.redirect = Some(RedirectOptions::disabled());
    EXPECT_FALSE(EffectiveOptions::resolve(defaults, options).unwrap().redirect().enabled());
    defaults.timeout.total = TimeoutLimit::after(Duration::from_millis(u64(100)));
    auto timed             = EffectiveOptions::resolve(defaults, RequestOptions {}).unwrap();
    EXPECT_TRUE(timed.remaining_after(Duration::from_millis(u64(101))).unwrap_err().is_Timeout());
}

TEST(http, LocalHttpRedirectPolicies) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto session = ncrequest::Session::make().unwrap();
    auto fetch   = [&](std::string path, RedirectOptions policy) {
        auto req         = make_request(local_http_url(base, path));
        auto options     = RequestOptions {};
        options.redirect = Some(policy);
        options.proxy    = Some(Proxy::disabled());
        auto timeout     = Timeout {};
        timeout.total    = TimeoutLimit::after(Duration::from_secs(u64(3)));
#ifndef LITO_FEAT_QT
        options.timeout = Some(timeout);
#endif
        req.set_options(rstd::move(options));
        return block_on(fetch_text_request(session.clone(), rstd::move(req)));
    };
    auto stopped = fetch("/redirect-to", RedirectOptions::disabled());
    EXPECT_EQ(stopped.code, 302);
    EXPECT_EQ(stopped.body, "redirect body");
    auto followed = fetch("/redirect-chain?left=2", RedirectOptions::same_origin(u32(2)));
    EXPECT_TRUE(followed.got_body) << followed.error;
    EXPECT_EQ(followed.body, "final");
    auto exceeded = fetch("/redirect-chain?left=2", RedirectOptions::same_origin(u32(1)));
    EXPECT_EQ(exceeded.protocol_error, ncrequest::ProtocolError::RedirectLimitExceeded);
    auto zero = fetch("/redirect-to", RedirectOptions::same_origin(u32()));
    EXPECT_EQ(zero.protocol_error, ncrequest::ProtocolError::RedirectLimitExceeded);
    auto cross =
        fetch("/redirect-to?to=http://other.invalid/secret", RedirectOptions::same_origin());
    EXPECT_EQ(cross.protocol_error, ncrequest::ProtocolError::RedirectOriginChanged);
    auto cross_port =
        fetch("/redirect-to?to=http://127.0.0.1:1/secret", RedirectOptions::same_origin());
    EXPECT_EQ(cross_port.protocol_error, ncrequest::ProtocolError::RedirectOriginChanged);
    auto backslash_cross =
        fetch("/redirect-to?to=%5C%5Cother.invalid%5Csecret", RedirectOptions::same_origin());
    EXPECT_EQ(backslash_cross.protocol_error, ncrequest::ProtocolError::RedirectOriginChanged);
    auto credentials =
        fetch("/redirect-to?to=http://user@127.0.0.1/secret", RedirectOptions::same_origin());
    EXPECT_EQ(credentials.protocol_error, ncrequest::ProtocolError::InvalidRedirect);
    auto scheme = fetch("/redirect-to?to=file:///tmp/private", RedirectOptions::same_origin());
    EXPECT_EQ(scheme.protocol_error, ncrequest::ProtocolError::RedirectOriginChanged);
    auto blob = fetch("/redirect-to?to=blob:http://127.0.0.1/id", RedirectOptions::same_origin());
    EXPECT_EQ(blob.protocol_error, ncrequest::ProtocolError::RedirectOriginChanged);
    auto invalid = fetch("/redirect-duplicate", RedirectOptions::same_origin());
#ifndef LITO_FEAT_QT
    EXPECT_EQ(invalid.client_code, static_cast<int>(curl::CURLcode::CURLE_WEIRD_SERVER_REPLY));
#else
    EXPECT_EQ(invalid.protocol_error, ncrequest::ProtocolError::InvalidRedirect);
#endif
    auto duplicate = fetch("/redirect-duplicate-same", RedirectOptions::same_origin());
    EXPECT_EQ(duplicate.protocol_error, ncrequest::ProtocolError::InvalidRedirect);
    auto relative = fetch("/redirect-to?to=./a/../text", RedirectOptions::same_origin());
    EXPECT_EQ(relative.code, 200);
    auto encoded_dot = fetch("/redirect-to?to=./a/%252e%252e/text", RedirectOptions::same_origin());
    EXPECT_EQ(encoded_dot.code, 200);
    auto normal = fetch("/redirect-to?code=300", RedirectOptions::same_origin());
    EXPECT_EQ(normal.code, 300);
    auto started = steady_clock::now();
    auto stall   = fetch("/redirect-stall", RedirectOptions::same_origin());
    EXPECT_TRUE(stall.got_body) << stall.error;
    EXPECT_LT(std::chrono::duration_cast<milliseconds>(steady_clock::now() - started).count(),
              2000);
}

TEST(http, LocalHttpRedirectMethodsAndBody) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto session = ncrequest::Session::make().unwrap();
    for (auto code : { 301, 302, 303, 307, 308 }) {
        for (auto method : { "POST", "PUT", "PATCH", "DELETE" }) {
            auto req =
                make_request(local_http_url(base, "/redirect-to?code=" + std::to_string(code)));
            ASSERT_TRUE(req.try_set_method(as_rstd_str(method)).is_ok());
            req.set_body(bytes_from_string("payload"));
            ASSERT_TRUE(req.try_set_header("Authorization"_str, "Bearer test"_str).is_ok());
            ASSERT_TRUE(req.try_set_header("Cookie"_str, "custom=test"_str).is_ok());
            ASSERT_TRUE(req.try_set_header("X-Api-Key"_str, "test-key"_str).is_ok());
            ASSERT_TRUE(req.try_set_header("Content-Type"_str, "test/type"_str).is_ok());
            auto options     = RequestOptions {};
            options.redirect = Some(RedirectOptions::same_origin());
            req.set_options(rstd::move(options));
            auto result = block_on(fetch_text_request(session.clone(), rstd::move(req)));
            ASSERT_TRUE(result.got_body) << result.error;
            bool get =
                code == 303 || ((code == 301 || code == 302) && std::string(method) == "POST");
            auto expected = get ? std::string("GET\n\n") : std::string(method) + "\npayload\n";
            expected += "Bearer test\ncustom=test\ntest-key\n";
            if (! get) expected += "test/type";
            EXPECT_EQ(result.body, expected);
        }
    }
#if ! defined(LITO_FEAT_QT)
    for (auto code : { 303, 307 }) {
        auto req = make_request(local_http_url(base, "/redirect-to?code=" + std::to_string(code)));
        req.try_set_method("POST"_str).unwrap();
        auto reader     = BodyReader {};
        reader.size     = Some(usize(1));
        reader.callback = [sent = false](byte* out, usize capacity) mutable -> usize {
            if (sent || capacity == usize()) return usize();
            *out = byte('x');
            sent = true;
            return usize(1);
        };
        req.set_body(RequestBody::from_reader(rstd::move(reader)).unwrap());
        auto options     = RequestOptions {};
        options.redirect = Some(RedirectOptions::same_origin());
        req.set_options(rstd::move(options));
        auto result = block_on(fetch_text_request(session.clone(), rstd::move(req)));
        if (code == 303)
            EXPECT_TRUE(result.got_body) << result.error;
        else
            EXPECT_EQ(result.protocol_error, ncrequest::ProtocolError::RedirectBodyNotReplayable);
    }
#endif
}

TEST(http, ResourceLimitsValidationAndSnapshot) {
    auto defaults                 = SessionOptions {};
    defaults.limits.collect_bytes = usize(7);
    auto request                  = RequestOptions {};
    auto inherited                = EffectiveOptions::resolve(defaults, request).unwrap();
    EXPECT_EQ(inherited.limits().collect_bytes.to_primitive(), 7u);
    request.limits                = Some(ncrequest::ResourceLimits {});
    auto snapshot                 = EffectiveOptions::resolve(defaults, request).unwrap();
    request.limits->collect_bytes = usize(2);
    EXPECT_EQ(snapshot.clone().limits().collect_bytes.to_primitive(), 8u * 1024u * 1024u);
    EXPECT_EQ(request.clone().limits->collect_bytes.to_primitive(), 2u);
    auto bad         = ncrequest::ResourceLimits {};
    bad.header_bytes = usize();
    EXPECT_TRUE(bad.validate().is_err());
    bad                      = ncrequest::ResourceLimits {};
    bad.receive_buffer_bytes = usize();
    EXPECT_TRUE(bad.validate().is_err());
    bad               = ncrequest::ResourceLimits {};
    bad.collect_bytes = usize();
    EXPECT_TRUE(bad.validate().is_ok());
}

TEST(http, LocalHttpHeaderResourceLimits) {
#ifndef LITO_FEAT_QT
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto session = ncrequest::Session::make().unwrap();
    auto fetch   = [&](const std::string& path, usize limit) {
        auto request                 = make_request(local_http_url(base, path));
        auto options                 = RequestOptions {};
        options.limits               = Some(ncrequest::ResourceLimits {});
        options.limits->header_bytes = limit;
        request.set_options(rstd::move(options));
        return block_on(fetch_text_request(session.clone(), rstd::move(request)));
    };
    auto exact = fetch("/limit-head", usize(86));
    EXPECT_TRUE(exact.got_body) << exact.error;
    for (auto path : { "/limit-head", "/limit-hints", "/limit-trailer" }) {
        auto result = fetch(path, usize(path == std::string("/limit-head") ? 85 : 128));
        EXPECT_TRUE(result.got_error) << path;
        EXPECT_EQ(result.protocol_error, ncrequest::ProtocolError::HeaderTooLarge) << result.error;
    }
    auto large = fetch("/limit-head?size=1024&count=70", usize(80 * 1024));
    EXPECT_TRUE(large.got_body) << large.error;
    auto default_cap = fetch("/limit-head?size=1024&count=70", usize(64 * 1024));
    EXPECT_EQ(default_cap.protocol_error, ncrequest::ProtocolError::HeaderTooLarge);
#else
    GTEST_SKIP();
#endif
}

namespace
{
auto collect_with_policy(Arc<ncrequest::Session> session, std::string url, bool take_body,
                         bool explicit_limit) -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value   = rstd::move(response).unwrap();
    auto collect = [&]() -> ncrequest::coro<ncrequest::Result<Bytes>> {
        if (! take_body) {
            if (explicit_limit) co_return co_await value->bytes(usize(4096));
            co_return co_await value->bytes();
        }
        auto body = value->take_body().unwrap();
        if (explicit_limit) co_return co_await body.collect(usize(4096));
        co_return co_await body.collect();
    };
    auto result = co_await collect();
    co_return result.is_err() && result.unwrap_err().is_Protocol() &&
        result.unwrap_err().as_Protocol().kind == ncrequest::ProtocolError::BodyTooLarge;
}

auto stream_with_policy(Arc<ncrequest::Session> session, std::string url) -> ncrequest::coro<bool> {
    auto response = co_await session->get(make_request(url));
    if (response.is_err()) co_return false;
    auto value = rstd::move(response).unwrap();
    auto body  = value->take_body().unwrap();
    co_await sleep(Duration::from_millis(u64(100)));
    if (value->is_finished()) co_return false;
    usize total {};
    for (;;) {
        auto part = co_await body.next();
        if (part.is_err()) co_return false;
        if (part->is_none()) break;
        total += (**part).size();
        co_await sleep(Duration::from_millis(u64(1)));
    }
    co_return total == usize(download_body().size());
}
} // namespace

TEST(http, LocalHttpCollectionResourceLimits) {
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto options                 = SessionOptions {};
    options.limits.collect_bytes = usize(16);
    auto session                 = ncrequest::Session::make(rstd::move(options)).unwrap();
    for (bool take_body : { false, true })
        for (bool explicit_limit : { false, true })
            EXPECT_TRUE(block_on(collect_with_policy(session.clone(),
                                                     local_http_url(base, "/limit-unknown"),
                                                     take_body,
                                                     explicit_limit)));
    auto inherited = block_on(fetch_text(session.clone(), local_http_url(base, "/text")));
    EXPECT_EQ(inherited.protocol_error, ncrequest::ProtocolError::BodyTooLarge);
    auto request     = make_request(local_http_url(base, "/text"));
    auto overrides   = RequestOptions {};
    overrides.limits = Some(ncrequest::ResourceLimits {});
    request.set_options(rstd::move(overrides));
    auto reset = block_on(fetch_text_request(session.clone(), rstd::move(request)));
    EXPECT_TRUE(reset.got_body) << reset.error;
    options                      = SessionOptions {};
    options.limits.collect_bytes = usize();
    auto zero                    = ncrequest::Session::make(rstd::move(options)).unwrap();
    auto empty = block_on(fetch_text(zero.clone(), local_http_url(base, "/empty")));
    EXPECT_TRUE(empty.got_body) << empty.error;
    auto rejected = block_on(fetch_text(zero.clone(), local_http_url(base, "/text")));
    EXPECT_EQ(rejected.protocol_error, ncrequest::ProtocolError::BodyTooLarge);
}

TEST(http, LocalHttpReceiveResourceLimits) {
#ifndef LITO_FEAT_QT
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    for (auto capacity : { 16384u, 32768u, 65536u }) {
        auto options                        = SessionOptions {};
        options.limits.receive_buffer_bytes = usize(capacity);
        options.limits.collect_bytes        = usize(1);
        auto session = ncrequest::Session::make(rstd::move(options)).unwrap();
        EXPECT_TRUE(
            block_on(stream_with_policy(session.clone(), local_http_url(base, "/download.bin"))));
    }
    auto options                        = SessionOptions {};
    options.limits.receive_buffer_bytes = usize(1);
    auto session                        = ncrequest::Session::make(rstd::move(options)).unwrap();
    auto result = block_on(fetch_text(session.clone(), local_http_url(base, "/text")));
    EXPECT_EQ(result.error_kind, ncrequest::ErrorKind::Unsupported);
#else
    GTEST_SKIP();
#endif
}

TEST(http, LocalHttpRedirectTotalBudget) {
#if ! defined(LITO_FEAT_QT)
    auto base = local_http_base_url();
    if (base.empty()) GTEST_SKIP();
    auto req         = make_request(local_http_url(base, "/redirect-chain?left=2&delay=0.12"));
    auto options     = RequestOptions {};
    auto timeout     = Timeout {};
    timeout.total    = TimeoutLimit::after(Duration::from_millis(u64(200)));
    options.timeout  = Some(timeout);
    options.redirect = Some(RedirectOptions::same_origin());
    req.set_options(rstd::move(options));
    auto result =
        block_on(fetch_text_request(ncrequest::Session::make().unwrap(), rstd::move(req)));
    EXPECT_TRUE(result.got_error);
    EXPECT_FALSE(result.got_body);
    EXPECT_EQ(result.error_kind, ncrequest::ErrorKind::Timeout) << result.error;
#else
    GTEST_SKIP() << "curl total timeout runtime test";
#endif
}
