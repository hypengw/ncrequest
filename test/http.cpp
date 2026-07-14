#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <rstd/enum.hpp>
#include <string>
#include <string_view>
import ncrequest;
import ncrequest.http.parser;
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
import ncrequest.curl;
#endif
import rstd;
import rstd.cppstd;

namespace
{

using ncrequest::byte;
using ncrequest::usize;

struct FetchResult {
    bool        got_response { false };
    bool        got_body { false };
    bool        got_error { false };
    int         code { 0 };
    bool        has_test_header { false };
    usize       set_cookie_count { 0 };
    bool        finished_while_paused { false };
    usize       upload_callback_count { 0 };
    usize       trailer_count { 0 };
    bool        initial_has_trailer { false };
    std::string body;
    std::string first_set_cookie_name;
    std::string repeated_header_values;
    std::string error;
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
    return rstd::move(ncrequest::Request::from_url(url)).unwrap();
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
        auto error        = rstd::move(rsp).unwrap_err();
        result.got_error  = true;
        result.error_kind = error.kind();
        result.error      = rstd::cppstd::to_string(rstd::format("{}", error));
        co_return result;
    }

    auto response       = rsp.unwrap();
    result.got_response = true;

    auto text = co_await response->text();
    if (text.is_err()) {
        result.error = rstd::cppstd::to_string(
            rstd::format("response text read failed: {}", text.unwrap_err()));
        co_return result;
    }

    result.code            = response_code(response);
    result.has_test_header = response->header().has_field("x-ncrequest-test");
    result.initial_has_trailer = response->header().has_field("x-ncrequest-trailer");
    result.body            = text.unwrap();
    auto trailers = response->trailers();
    if (trailers.is_some()) {
        result.trailer_count = (**trailers).values("x-ncrequest-trailer").count();
    }
    auto set_cookies = response->set_cookies();
    if (set_cookies.is_err()) {
        result.error = "Set-Cookie parsing failed";
        co_return result;
    }
    auto cookies = rstd::move(set_cookies).unwrap();
    result.set_cookie_count = cookies.len();
    if (! cookies.is_empty()) {
        result.first_set_cookie_name =
            rstd::cppstd::to_string(cookies[0].cookie().name());
    }
    auto repeated = response->header().values("x-ncrequest-repeat");
    for (auto value = repeated.next(); value.is_some(); value = repeated.next()) {
        auto text_value = (**value).as_str();
        if (text_value.is_none()) {
            result.error = "repeated response header is not UTF-8";
            co_return result;
        }
        if (! result.repeated_header_values.empty()) {
            result.repeated_header_values.push_back('|');
        }
        result.repeated_header_values.append(rstd::cppstd::as_string_view(*text_value));
    }
    result.got_body        = true;
    co_return result;
}

auto fetch_text(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    return fetch_text_request(rstd::move(session), make_request(url));
}

auto request_with_share(std::string url, const ncrequest::SessionShare& share)
    -> ncrequest::Request {
    auto request = make_request(url);
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
    auto        req = make_request(url);
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
    auto        req = make_request(url);
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
    auto        req = make_request(url);
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
    return timeout_request(rstd::move(session), make_request(url));
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
    return cancel_request(rstd::move(session), make_request(url));
}

#ifdef NCREQUEST_CLIENT_BACKEND_CURL
auto curl_pause_recv(ncrequest::Arc<ncrequest::Session> session, std::string url)
    -> ncrequest::coro<FetchResult> {
    FetchResult result;
    auto        req = make_request(url);
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
    auto        req    = make_request(url);
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
    using namespace ncrequest::http;

    EXPECT_EQ(rstd::cppstd::as_string_view(encode_component("a b/+~").as_str()),
              "a%20b%2F%2B~");

    auto decoded = decode_component("a%20b%2Fb+plus");
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(decoded.unwrap().as_str()), "a b/b+plus");

    auto invalid = decode_component("a%20b%ZZ");
    ASSERT_TRUE(invalid.is_err());
    EXPECT_TRUE(invalid.unwrap_err().kind().is_InvalidPercentEncoding());
    EXPECT_EQ(invalid.unwrap_err().offset(), 6u);

    auto form = decode_form_component("a+b");
    ASSERT_TRUE(form.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(form.unwrap().as_str()), "a b");
}

TEST(http, QueryParamsPreserveOrderedRepeatedValues) {
    using ncrequest::http::QueryParams;

    auto parsed = QueryParams::parse_form("first=one&repeat=a&empty=&repeat=b+c");
    ASSERT_TRUE(parsed.is_ok());
    auto query = rstd::move(parsed).unwrap();

    EXPECT_EQ(query.len(), 4u);
    EXPECT_EQ(rstd::cppstd::as_string_view(*query.get("first")), "one");

    auto values = query.values("repeat");
    auto first  = values.next();
    auto second = values.next();
    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*first), "a");
    EXPECT_EQ(rstd::cppstd::as_string_view(*second), "b c");
    EXPECT_TRUE(values.next().is_none());

    EXPECT_EQ(rstd::cppstd::as_string_view(query.encode_form().as_str()),
              "first=one&repeat=a&empty=&repeat=b+c");

    query.set("repeat", "replacement");
    EXPECT_EQ(rstd::cppstd::as_string_view(*query.get("repeat")), "replacement");
    EXPECT_EQ(query.values("repeat").next()->size(), 11u);

    auto invalid_utf8 = QueryParams::parse_form("key=%FF");
    ASSERT_TRUE(invalid_utf8.is_err());
    EXPECT_TRUE(invalid_utf8.unwrap_err().kind().is_InvalidUtf8());
    EXPECT_EQ(invalid_utf8.unwrap_err().offset(), 4u);

    auto from_trait = rstd::from_str<QueryParams>("a=1&a=2");
    ASSERT_TRUE(from_trait.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", from_trait.unwrap())), "a=1&a=2");

    auto raw = QueryParams::parse_query("value=a+b&space=a%20b");
    ASSERT_TRUE(raw.is_ok());
    auto raw_query = rstd::move(raw).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(*raw_query.get("value")), "a+b");
    EXPECT_EQ(rstd::cppstd::as_string_view(*raw_query.get("space")), "a b");
    EXPECT_EQ(rstd::cppstd::as_string_view(raw_query.encode_query().as_str()),
              "value=a%2Bb&space=a%20b");

    auto raw_trait = rstd::from_str<QueryParams>("value=a+b");
    ASSERT_TRUE(raw_trait.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", raw_trait.unwrap())),
              "value=a%2Bb");
}

TEST(http, CookieValuesParseAttributesAndPreserveOrder) {
    using namespace ncrequest::http;

    auto pair = rstd::from_str<Cookie>("session=\"abc123\"");
    ASSERT_TRUE(pair.is_ok());
    auto cookie = rstd::move(pair).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(cookie.name()), "session");
    EXPECT_EQ(rstd::cppstd::as_string_view(cookie.value()), "abc123");
    EXPECT_TRUE(cookie.is_quoted());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", cookie)),
              "session=\"abc123\"");

    auto header = CookieHeader::parse(" \tfirst=one; repeat=a; repeat=\"b\" \t");
    ASSERT_TRUE(header.is_ok());
    auto cookies = rstd::move(header).unwrap();
    EXPECT_EQ(cookies.len(), 3u);
    auto repeat = cookies.get("repeat");
    ASSERT_TRUE(repeat.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view((**repeat).value()), "a");
    EXPECT_TRUE(cookies.get("Repeat").is_none());
    EXPECT_EQ(rstd::cppstd::as_string_view(cookies.encode().as_str()),
              "first=one; repeat=a; repeat=\"b\"");

    auto set = SetCookie::parse(
        "session=abc123; Path=/account; Secure; HttpOnly; SameSite=Lax");
    ASSERT_TRUE(set.is_ok());
    auto set_cookie = rstd::move(set).unwrap();
    EXPECT_TRUE(set_cookie.secure());
    EXPECT_TRUE(set_cookie.http_only());
    auto path = set_cookie.attribute("path");
    ASSERT_TRUE(path.is_some());
    ASSERT_TRUE((**path).value().is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**path).value()), "/account");
    EXPECT_EQ(set_cookie.attributes().count(), 4u);
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", set_cookie)),
              "session=abc123; Path=/account; Secure; HttpOnly; SameSite=Lax");

    auto invalid_name = Cookie::parse("bad name=value");
    ASSERT_TRUE(invalid_name.is_err());
    EXPECT_TRUE(invalid_name.unwrap_err().kind().is_InvalidName());
    EXPECT_EQ(invalid_name.unwrap_err().offset(), 3u);

    auto invalid_value = Cookie::parse("name=a,b");
    ASSERT_TRUE(invalid_value.is_err());
    EXPECT_TRUE(invalid_value.unwrap_err().kind().is_InvalidValue());
    EXPECT_EQ(invalid_value.unwrap_err().offset(), 6u);

    auto trailing = CookieHeader::parse("a=1;");
    ASSERT_TRUE(trailing.is_err());
    EXPECT_TRUE(trailing.unwrap_err().kind().is_EmptyName());
    EXPECT_EQ(trailing.unwrap_err().offset(), 4u);
}

TEST(http, ParserCursorCompositionAndErrors) {
    namespace parser = ncrequest::http::parser;

    auto cursor = parser::Cursor { "alpha" };
    auto prefix = parser::take_literal(cursor, "alp");
    ASSERT_TRUE(prefix.is_ok());
    EXPECT_EQ(prefix.unwrap().begin, 0u);
    EXPECT_EQ(prefix.unwrap().end, 3u);
    EXPECT_EQ(cursor.offset(), 3u);
    EXPECT_EQ(rstd::cppstd::as_string_view(
                  *rstd::str_::from_utf8(cursor.slice(parser::Span { 0, 3 }))),
              "alp");

    auto incomplete = parser::take_literal(cursor, "habet");
    ASSERT_TRUE(incomplete.is_err());
    auto incomplete_error = rstd::move(incomplete).unwrap_err();
    EXPECT_TRUE(incomplete_error.is_incomplete());
    EXPECT_FALSE(incomplete_error.is_committed());
    EXPECT_EQ(incomplete_error.offset(), 5u);
    EXPECT_EQ(cursor.offset(), 3u);

    auto uncommitted_cursor = parser::Cursor { "ac" };
    auto uncommitted = parser::choice(
        uncommitted_cursor,
        [](parser::Cursor& input) -> parser::ParseResult<parser::Span> {
            auto begin = input.mark();
            auto first = parser::take_byte(input, 'a');
            if (first.is_err()) return rstd::Err(rstd::move(first).unwrap_err());
            auto second = parser::take_byte(input, 'b');
            if (second.is_err()) return rstd::Err(rstd::move(second).unwrap_err());
            return rstd::Ok(input.span_from(begin));
        },
        [](parser::Cursor& input) {
            return parser::take_literal(input, "ac");
        });
    ASSERT_TRUE(uncommitted.is_ok());
    EXPECT_EQ(uncommitted_cursor.offset(), 2u);

    auto committed_cursor = parser::Cursor { "ac" };
    auto committed = parser::choice(
        committed_cursor,
        [](parser::Cursor& input) -> parser::ParseResult<parser::Span> {
            auto begin = input.mark();
            auto first = parser::take_byte(input, 'a');
            if (first.is_err()) return rstd::Err(rstd::move(first).unwrap_err());
            auto second = parser::committed(parser::take_byte(input, 'b'));
            if (second.is_err()) return rstd::Err(rstd::move(second).unwrap_err());
            return rstd::Ok(input.span_from(begin));
        },
        [](parser::Cursor& input) {
            return parser::take_literal(input, "ac");
        });
    ASSERT_TRUE(committed.is_err());
    auto committed_error = rstd::move(committed).unwrap_err();
    EXPECT_TRUE(committed_error.is_committed());
    EXPECT_EQ(committed_error.offset(), 1u);
    EXPECT_EQ(committed_cursor.offset(), 1u);

    auto attempted_cursor = parser::Cursor { "ac" };
    auto attempted = parser::attempt(
        attempted_cursor,
        [](parser::Cursor& input) -> parser::ParseResult<parser::Span> {
            auto begin = input.mark();
            auto first = parser::take_byte(input, 'a');
            if (first.is_err()) return rstd::Err(rstd::move(first).unwrap_err());
            auto second = parser::take_byte(input, 'b');
            if (second.is_err()) return rstd::Err(rstd::move(second).unwrap_err());
            return rstd::Ok(input.span_from(begin));
        });
    ASSERT_TRUE(attempted.is_err());
    EXPECT_EQ(attempted_cursor.offset(), 0u);

    auto optional_cursor = parser::Cursor { "?value" };
    auto present = parser::optional(optional_cursor, [](parser::Cursor& input) {
        return parser::take_byte(input, '?');
    });
    ASSERT_TRUE(present.is_ok());
    EXPECT_TRUE(present.unwrap().is_some());
    EXPECT_EQ(optional_cursor.offset(), 1u);
    auto absent = parser::optional(optional_cursor, [](parser::Cursor& input) {
        return parser::take_byte(input, '#');
    });
    ASSERT_TRUE(absent.is_ok());
    EXPECT_TRUE(absent.unwrap().is_none());
    EXPECT_EQ(optional_cursor.offset(), 1u);

    auto partial_cursor = parser::Cursor { "aX" };
    auto partial = parser::optional(partial_cursor, [](parser::Cursor& input) {
        return parser::take_literal(input, "ab");
    });
    ASSERT_TRUE(partial.is_err());
    EXPECT_EQ(partial.unwrap_err().offset(), 1u);
    EXPECT_EQ(partial_cursor.offset(), 0u);

    auto sequence_cursor = parser::Cursor { "\r\nrest" };
    auto sequenced = parser::sequence(
        sequence_cursor,
        [](parser::Cursor& input) { return parser::take_byte(input, '\r'); },
        [](parser::Cursor& input) { return parser::take_byte(input, '\n'); });
    ASSERT_TRUE(sequenced.is_ok());
    EXPECT_EQ(sequenced.unwrap().size(), 2u);

    auto repeat_cursor = parser::Cursor { "///path" };
    auto repeated = parser::repeat(
        repeat_cursor,
        [](parser::Cursor& input) { return parser::take_byte(input, '/'); }, 1, 4);
    ASSERT_TRUE(repeated.is_ok());
    EXPECT_EQ(repeated.unwrap().size(), 3u);
    EXPECT_EQ(repeat_cursor.offset(), 3u);

    auto delimited_cursor = parser::Cursor { "[ok]" };
    auto delimited = parser::delimited(
        delimited_cursor,
        [](parser::Cursor& input) { return parser::take_byte(input, '['); },
        [](parser::Cursor& input) { return parser::take_literal(input, "ok"); },
        [](parser::Cursor& input) {
            return parser::committed(parser::take_byte(input, ']'));
        });
    ASSERT_TRUE(delimited.is_ok());
    EXPECT_EQ(delimited.unwrap().size(), 2u);
    EXPECT_EQ(delimited_cursor.offset(), 4u);

    auto unclosed_cursor = parser::Cursor { "[ok" };
    auto unclosed = parser::delimited(
        unclosed_cursor,
        [](parser::Cursor& input) { return parser::take_byte(input, '['); },
        [](parser::Cursor& input) { return parser::take_literal(input, "ok"); },
        [](parser::Cursor& input) {
            return parser::committed(parser::take_byte(input, ']'));
        });
    ASSERT_TRUE(unclosed.is_err());
    EXPECT_TRUE(unclosed.unwrap_err().is_committed());
}

TEST(http, UrlParsesOwnedComponentsAndTraits) {
    using ncrequest::http::Url;

    auto parsed = Url::parse("foo://user@example.com:8042/over/there?name=ferret#nose");
    ASSERT_TRUE(parsed.is_ok());
    auto url = rstd::move(parsed).unwrap();

    EXPECT_EQ(rstd::cppstd::as_string_view(*url.scheme()), "foo");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.authority()), "user@example.com:8042");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.userinfo()), "user");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.host()), "example.com");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.port()), "8042");
    EXPECT_EQ(rstd::cppstd::as_string_view(url.path()), "/over/there");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.query()), "name=ferret");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.fragment()), "nose");
    EXPECT_EQ(rstd::cppstd::to_string(url.request_target()), "/over/there?name=ferret");

    auto cloned = rstd::as<rstd::clone::Clone>(url).clone();
    EXPECT_EQ(rstd::cppstd::as_string_view(cloned.as_ref()),
              "foo://user@example.com:8042/over/there?name=ferret#nose");
    EXPECT_EQ(rstd::cppstd::as_string_view(
                  rstd::as<rstd::convert::AsRef<rstd::str>>(cloned).as_ref()),
              rstd::cppstd::as_string_view(cloned.as_ref()));
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", cloned)),
              "foo://user@example.com:8042/over/there?name=ferret#nose");

    auto from_trait = rstd::from_str<Url>("../relative?");
    ASSERT_TRUE(from_trait.is_ok());
    auto relative = rstd::move(from_trait).unwrap();
    ASSERT_TRUE(relative.query().is_some());
    EXPECT_EQ(relative.query()->size(), 0u);
    EXPECT_TRUE(relative.fragment().is_none());

    auto empty_fragment = Url::parse("#");
    ASSERT_TRUE(empty_fragment.is_ok());
    auto empty_fragment_url = rstd::move(empty_fragment).unwrap();
    EXPECT_TRUE(empty_fragment_url.query().is_none());
    ASSERT_TRUE(empty_fragment_url.fragment().is_some());
    EXPECT_EQ(empty_fragment_url.fragment()->size(), 0u);

    auto encoded_path = Url::parse("http://example.com/a%2Fb");
    ASSERT_TRUE(encoded_path.is_ok());
    auto encoded_path_url = rstd::move(encoded_path).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(encoded_path_url.path()), "/a%2Fb");
    EXPECT_EQ(rstd::cppstd::to_string(encoded_path_url.request_target()), "/a%2Fb");
}

TEST(http, HttpUrlValidationReportsKindsAndOffsets) {
    using ncrequest::http::Url;

    auto invalid_percent = Url::parse("http://example.com/%zz");
    ASSERT_TRUE(invalid_percent.is_err());
    auto percent_error = rstd::move(invalid_percent).unwrap_err();
    EXPECT_TRUE(percent_error.kind().is_InvalidPercentEncoding());
    EXPECT_EQ(percent_error.offset(), 19u);

    auto invalid_character = Url::parse("http://example.com/a b");
    ASSERT_TRUE(invalid_character.is_err());
    auto character_error = rstd::move(invalid_character).unwrap_err();
    EXPECT_TRUE(character_error.kind().is_InvalidCharacter());
    EXPECT_EQ(character_error.offset(), 20u);

    auto missing_scheme = Url::parse_http("//example.com/path");
    ASSERT_TRUE(missing_scheme.is_err());
    EXPECT_TRUE(missing_scheme.unwrap_err().kind().is_MissingScheme());

    auto unsupported = Url::parse_http("ftp://example.com/path");
    ASSERT_TRUE(unsupported.is_err());
    EXPECT_TRUE(unsupported.unwrap_err().kind().is_UnsupportedScheme());

    auto missing_authority = Url::parse_http("http:path");
    ASSERT_TRUE(missing_authority.is_err());
    EXPECT_TRUE(missing_authority.unwrap_err().kind().is_MissingAuthority());

    auto missing_host = Url::parse_http("http:///path");
    ASSERT_TRUE(missing_host.is_err());
    EXPECT_TRUE(missing_host.unwrap_err().kind().is_MissingHost());

    auto request =
        ncrequest::Request::from_url("https://[2001:db8::1]/resource?#fragment");
    if (request.is_err()) {
        auto error = rstd::move(request).unwrap_err();
        FAIL() << "URL error kind " << error.kind().index() << " at " << error.offset();
    }
    auto value = rstd::move(request).unwrap();
    EXPECT_EQ(value.url(), "https://[2001:db8::1]/resource?#fragment");
    EXPECT_EQ(rstd::cppstd::to_string(value.url_info().request_target()), "/resource?");
}

TEST(http, UrlResolvesRfc3986References) {
    using ncrequest::http::Url;

    auto base_result = Url::parse("http://a/b/c/d;p?q");
    ASSERT_TRUE(base_result.is_ok());
    auto base = rstd::move(base_result).unwrap();

    struct Example {
        const char* reference;
        const char* expected;
    };
    constexpr Example examples[] = {
        { "g:h", "g:h" },
        { "g", "http://a/b/c/g" },
        { "./g", "http://a/b/c/g" },
        { "g/", "http://a/b/c/g/" },
        { "/g", "http://a/g" },
        { "//g", "http://g" },
        { "?y", "http://a/b/c/d;p?y" },
        { "g?y", "http://a/b/c/g?y" },
        { "#s", "http://a/b/c/d;p?q#s" },
        { "g#s", "http://a/b/c/g#s" },
        { "g?y#s", "http://a/b/c/g?y#s" },
        { ";x", "http://a/b/c/;x" },
        { "g;x", "http://a/b/c/g;x" },
        { "g;x?y#s", "http://a/b/c/g;x?y#s" },
        { "", "http://a/b/c/d;p?q" },
        { ".", "http://a/b/c/" },
        { "./", "http://a/b/c/" },
        { "..", "http://a/b/" },
        { "../", "http://a/b/" },
        { "../g", "http://a/b/g" },
        { "../..", "http://a/" },
        { "../../", "http://a/" },
        { "../../g", "http://a/g" },
        { "../../../g", "http://a/g" },
        { "../../../../g", "http://a/g" },
        { "/./g", "http://a/g" },
        { "/../g", "http://a/g" },
        { "g.", "http://a/b/c/g." },
        { ".g", "http://a/b/c/.g" },
        { "g..", "http://a/b/c/g.." },
        { "..g", "http://a/b/c/..g" },
        { "./../g", "http://a/b/g" },
        { "./g/.", "http://a/b/c/g/" },
        { "g/./h", "http://a/b/c/g/h" },
        { "g/../h", "http://a/b/c/h" },
        { "g;x=1/./y", "http://a/b/c/g;x=1/y" },
        { "g;x=1/../y", "http://a/b/c/y" },
        { "g?y/./x", "http://a/b/c/g?y/./x" },
        { "g?y/../x", "http://a/b/c/g?y/../x" },
        { "g#s/./x", "http://a/b/c/g#s/./x" },
        { "g#s/../x", "http://a/b/c/g#s/../x" },
        { "http:g", "http:g" },
    };

    for (auto const& example : examples) {
        auto reference = Url::parse(example.reference);
        ASSERT_TRUE(reference.is_ok()) << example.reference;
        auto resolved = base.resolve(reference.unwrap());
        ASSERT_TRUE(resolved.is_ok()) << example.reference;
        EXPECT_EQ(rstd::cppstd::as_string_view(resolved.unwrap().as_ref()), example.expected)
            << example.reference;
    }
}

TEST(http, UriParserValidatesIpLiterals) {
    using ncrequest::http::Url;

    constexpr const char* valid[] = {
        "http://[::]/",
        "http://[::1]/",
        "http://[2001:db8::1]/",
        "http://[1:2:3:4:5:6:7:8]/",
        "http://[::ffff:192.0.2.1]/",
        "http://[v1.fe80::a]/",
    };
    for (auto value : valid) {
        auto parsed = Url::parse_http(value);
        EXPECT_TRUE(parsed.is_ok()) << value;
    }

    constexpr const char* invalid[] = {
        "http://[:]/",
        "http://[1:2:3:4:5:6:7]/",
        "http://[1:2:3:4:5:6:7:8:9]/",
        "http://[1::2::3]/",
        "http://[::ffff:999.0.2.1]/",
        "http://[v.fe80]/",
        "http://[v1.]/",
    };
    for (auto value : invalid) {
        auto parsed = Url::parse_http(value);
        ASSERT_TRUE(parsed.is_err()) << value;
        EXPECT_TRUE(parsed.unwrap_err().kind().is_InvalidIpAddress()) << value;
    }

    auto invalid_ipv4 = Url::parse_http("http://999.0.2.1/");
    ASSERT_TRUE(invalid_ipv4.is_err());
    EXPECT_TRUE(invalid_ipv4.unwrap_err().kind().is_InvalidIpAddress());
    EXPECT_EQ(invalid_ipv4.unwrap_err().offset(), 7u);

    auto invalid_port = Url::parse_http("http://example.com:65536/");
    ASSERT_TRUE(invalid_port.is_err());
    EXPECT_TRUE(invalid_port.unwrap_err().kind().is_InvalidPort());
    EXPECT_EQ(invalid_port.unwrap_err().offset(), 23u);
}

TEST(http, HeaderPreservesCaseInsensitiveOrderedValues) {
    using ncrequest::http::Header;
    using ncrequest::http::HeaderName;
    using ncrequest::http::HeaderValue;

    auto parsed_name = rstd::from_str<HeaderName>("Set-Cookie");
    ASSERT_TRUE(parsed_name.is_ok());
    auto name = rstd::move(parsed_name).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(
                  rstd::as<rstd::convert::AsRef<rstd::str>>(name).as_ref()),
              "Set-Cookie");

    auto parsed_value = rstd::from_str<HeaderValue>("first=1");
    ASSERT_TRUE(parsed_value.is_ok());
    auto value = rstd::move(parsed_value).unwrap();
    ASSERT_TRUE(value.as_str().is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*value.as_str()), "first=1");

    auto headers = Header {};
    ASSERT_TRUE(headers.add(name.as_ref(), rstd::move(value)).is_ok());
    ASSERT_TRUE(headers.add("set-cookie", "second=2").is_ok());
    ASSERT_TRUE(headers.add("Foo", "one").is_ok());
    ASSERT_TRUE(headers.add("Foobar", "two").is_ok());

    EXPECT_TRUE(headers.contains("SET-COOKIE"));
    EXPECT_TRUE(headers.contains("foo"));
    EXPECT_TRUE(headers.contains("foobar"));
    EXPECT_FALSE(headers.contains("fo"));
    ASSERT_TRUE(headers.get("set-cookie").is_some());
    auto first_header = headers.get("set-cookie");
    ASSERT_TRUE(first_header.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**first_header).as_str()), "first=1");

    auto values = headers.values("SET-cookie");
    auto first  = values.next();
    auto second = values.next();
    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**first).as_str()), "first=1");
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**second).as_str()), "second=2");
    EXPECT_TRUE(values.next().is_none());

    ASSERT_TRUE(headers.set("fOo", "replacement").is_ok());
    EXPECT_EQ(headers.len(), 4u);
    EXPECT_EQ(headers.values("foo").count(), 1u);
    auto replacement = headers.get("foo");
    ASSERT_TRUE(replacement.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**replacement).as_str()), "replacement");

    auto fields = headers.iter();
    auto field0 = fields.next();
    auto field1 = fields.next();
    auto field2 = fields.next();
    auto field3 = fields.next();
    ASSERT_TRUE(field0.is_some());
    ASSERT_TRUE(field1.is_some());
    ASSERT_TRUE(field2.is_some());
    ASSERT_TRUE(field3.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view((**field0).name().as_ref()), "Set-Cookie");
    EXPECT_EQ(rstd::cppstd::as_string_view((**field1).name().as_ref()), "set-cookie");
    EXPECT_EQ(rstd::cppstd::as_string_view((**field2).name().as_ref()), "Foobar");
    EXPECT_EQ(rstd::cppstd::as_string_view((**field3).name().as_ref()), "fOo");
}

TEST(http, HeaderRejectsInvalidNamesAndValues) {
    using ncrequest::http::HeaderName;
    using ncrequest::http::HeaderValue;

    auto empty_name = HeaderName::parse("");
    ASSERT_TRUE(empty_name.is_err());
    EXPECT_TRUE(empty_name.unwrap_err().kind().is_InvalidName());
    EXPECT_EQ(empty_name.unwrap_err().offset(), 0u);

    auto invalid_name = HeaderName::parse("Bad Name");
    ASSERT_TRUE(invalid_name.is_err());
    EXPECT_TRUE(invalid_name.unwrap_err().kind().is_InvalidName());
    EXPECT_EQ(invalid_name.unwrap_err().offset(), 3u);

    auto line_break = HeaderValue::parse("safe\r\ninjected");
    ASSERT_TRUE(line_break.is_err());
    EXPECT_TRUE(line_break.unwrap_err().kind().is_InvalidLineBreak());
    EXPECT_EQ(line_break.unwrap_err().offset(), 4u);

    const rstd::u8 control_bytes[] = { 'a', 0, 'b' };
    auto control = HeaderValue::from_bytes(
        rstd::slice<rstd::u8>::from_raw_parts(control_bytes, 3));
    ASSERT_TRUE(control.is_err());
    EXPECT_TRUE(control.unwrap_err().kind().is_InvalidValue());
    EXPECT_EQ(control.unwrap_err().offset(), 1u);

    const rstd::u8 opaque_bytes[] = { 'a', 0xff, 'b' };
    auto opaque = HeaderValue::from_bytes(
        rstd::slice<rstd::u8>::from_raw_parts(opaque_bytes, 3));
    ASSERT_TRUE(opaque.is_ok());
    auto opaque_value = rstd::move(opaque).unwrap();
    EXPECT_TRUE(opaque_value.as_str().is_none());
    EXPECT_EQ(opaque_value.as_bytes().len(), 3u);
}

TEST(http, HeaderCloneAndRequestReuseTypedOwner) {
    auto source = ncrequest::http::Header {};
    ASSERT_TRUE(source.add("X-First", "one").is_ok());
    ASSERT_TRUE(source.add("Set-Cookie", "a=1").is_ok());
    ASSERT_TRUE(source.add("set-cookie", "b=2").is_ok());

    auto cloned = rstd::as<rstd::clone::Clone>(source).clone();
    ASSERT_TRUE(cloned.set("X-First", "changed").is_ok());
    auto source_first = source.get("x-first");
    auto cloned_first = cloned.get("x-first");
    ASSERT_TRUE(source_first.is_some());
    ASSERT_TRUE(cloned_first.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**source_first).as_str()), "one");
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**cloned_first).as_str()), "changed");

    auto request = ncrequest::Request {};
    request.update_header(source);
    EXPECT_EQ(request.header("x-first"), "one");
    EXPECT_EQ(request.header().values("set-cookie").count(), 2u);
    ASSERT_TRUE(request.try_set_header("X-First", "request").is_ok());
    EXPECT_EQ(request.header("x-first"), "request");

    auto request_clone = request.clone();
    ASSERT_TRUE(request_clone.try_set_header("X-First", "clone").is_ok());
    EXPECT_EQ(request.header("x-first"), "request");
    EXPECT_EQ(request_clone.header("x-first"), "clone");
}

TEST(http, MessageHeadParsesTypedResponseAndDuplicateFields) {
    using ncrequest::http::MessageHead;

    auto parsed = MessageHead::parse(rstd::str_::as_bytes(
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: a=1\r\n"
        "set-cookie:\tb=2 \t\r\n"
        "X-Empty:\r\n"
        "\r\n"));
    ASSERT_TRUE(parsed.is_ok());
    auto head = rstd::move(parsed).unwrap();

    ASSERT_TRUE(head.start().is_Response());
    auto const& status = head.start().as_Response().value;
    EXPECT_EQ(status.status().value(), 200u);
    auto version = status.version();
    ASSERT_TRUE(version.is_some());
    EXPECT_EQ(version->major(), 1u);
    EXPECT_EQ(version->minor(), 1u);
    auto reason = status.reason();
    ASSERT_TRUE(reason.is_some());
    ASSERT_TRUE((**reason).as_str().is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**reason).as_str()), "OK");

    EXPECT_EQ(head.status_code().unwrap(), 200u);
    EXPECT_EQ(head.headers().values("set-cookie").count(), 2u);
    EXPECT_TRUE(head.has_field("x-empty"));
    auto empty = head.headers().get("X-Empty");
    ASSERT_TRUE(empty.is_some());
    EXPECT_EQ((**empty).as_bytes().len(), 0u);

    auto clone = head.clone();
    EXPECT_EQ(clone.status_code().unwrap(), 200u);
    EXPECT_EQ(clone.headers().values("SET-COOKIE").count(), 2u);

    auto saw_response = false;
    auto start = head.start().clone();
    RSTD_MATCH(rstd::move(start)) {
        RSTD_CASE(Request, value) {
            (void)value;
            break;
        }
        RSTD_CASE(Response, value) {
            saw_response = value.status().value() == 200u;
            break;
        }
    }
    EXPECT_TRUE(saw_response);
}

TEST(http, Http1HeadParserComposesAcrossArbitraryChunks) {
    auto parser = ncrequest::http::Http1HeadParser {};
    auto first  = parser.push(rstd::str_::as_bytes("HTTP/1.1 204 No"));
    ASSERT_TRUE(first.is_ok());
    EXPECT_TRUE(first.unwrap().is_NeedMore());

    auto second = parser.push(rstd::str_::as_bytes(" Content\r\nX-Test"));
    ASSERT_TRUE(second.is_ok());
    EXPECT_TRUE(second.unwrap().is_NeedMore());

    auto third = parser.push(rstd::str_::as_bytes(": value\r\n\r\n"));
    ASSERT_TRUE(third.is_ok());
    auto event = rstd::move(third).unwrap();
    ASSERT_TRUE(event.is_Complete());
    auto completed = rstd::move(event).as_Complete();
    auto head = rstd::move(completed.head);
    EXPECT_EQ(head.status_code().unwrap(), 204u);
    EXPECT_TRUE(head.has_field("x-test"));

    auto incomplete = ncrequest::http::Http1HeadParser {};
    auto partial = incomplete.push(rstd::str_::as_bytes("HTTP/1.1 200 OK\r\nX: value"));
    ASSERT_TRUE(partial.is_ok());
    EXPECT_TRUE(partial.unwrap().is_NeedMore());
    auto ended = incomplete.finish();
    ASSERT_TRUE(ended.is_err());
    EXPECT_TRUE(ended.unwrap_err().kind().is_UnexpectedEof());
    EXPECT_EQ(ended.unwrap_err().offset(), 25u);

    constexpr auto head_with_body = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody";
    constexpr auto head_size = sizeof("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n") - 1;
    auto followed = ncrequest::http::Http1HeadParser {};
    auto followed_result = followed.push(rstd::str_::as_bytes(head_with_body));
    ASSERT_TRUE(followed_result.is_ok());
    auto followed_event = rstd::move(followed_result).unwrap();
    ASSERT_TRUE(followed_event.is_Complete());
    auto followed_complete = rstd::move(followed_event).as_Complete();
    EXPECT_EQ(followed_complete.consumed, head_size);
    EXPECT_EQ(followed_complete.head.status_code().unwrap(), 200u);

    auto exact = ncrequest::http::MessageHead::parse(rstd::str_::as_bytes(head_with_body));
    ASSERT_TRUE(exact.is_err());
    EXPECT_TRUE(exact.unwrap_err().kind().is_InvalidSyntax());
    EXPECT_EQ(exact.unwrap_err().offset(), head_size);

    auto oversized = std::string(ncrequest::http::Http1HeadParser::MaxHeaderBytes + 1, 'x');
    auto oversized_parser = ncrequest::http::Http1HeadParser {};
    auto oversized_result = oversized_parser.push(rstd::str_::as_bytes(oversized));
    ASSERT_TRUE(oversized_result.is_err());
    EXPECT_TRUE(oversized_result.unwrap_err().kind().is_HeaderTooLarge());
    EXPECT_EQ(oversized_result.unwrap_err().offset(),
              ncrequest::http::Http1HeadParser::MaxHeaderBytes);

    auto large_body_input = std::string("HTTP/1.1 200 OK\r\n\r\n");
    large_body_input.append(ncrequest::http::Http1HeadParser::MaxHeaderBytes + 1, 'x');
    auto large_body_parser = ncrequest::http::Http1HeadParser {};
    auto large_body_result = large_body_parser.push(rstd::str_::as_bytes(large_body_input));
    ASSERT_TRUE(large_body_result.is_ok());
    EXPECT_TRUE(large_body_result.unwrap().is_Complete());
}

TEST(http, Http1FieldSectionParserKeepsTrailersSeparate) {
    auto parser = ncrequest::http::Http1FieldSectionParser {};
    auto first = parser.push(rstd::str_::as_bytes("Digest: first\r\nX-Tra"));
    ASSERT_TRUE(first.is_ok());
    EXPECT_TRUE(first.unwrap().is_NeedMore());

    constexpr auto remainder = "iler: second\r\n\r\nbody";
    auto second = parser.push(rstd::str_::as_bytes(remainder));
    ASSERT_TRUE(second.is_ok());
    auto event = rstd::move(second).unwrap();
    ASSERT_TRUE(event.is_Complete());
    auto complete = rstd::move(event).as_Complete();
    EXPECT_EQ(complete.consumed,
              sizeof("Digest: first\r\nX-Trailer: second\r\n\r\n") - 1);
    EXPECT_EQ(complete.fields.len(), 2u);
    EXPECT_TRUE(complete.fields.contains("digest"));
    EXPECT_TRUE(complete.fields.contains("x-trailer"));

    auto initial = ncrequest::http::MessageHead::parse(
        rstd::str_::as_bytes("HTTP/1.1 200 OK\r\nX-Initial: value\r\n\r\n"));
    ASSERT_TRUE(initial.is_ok());
    EXPECT_TRUE(initial.unwrap().headers().contains("x-initial"));
    EXPECT_FALSE(initial.unwrap().headers().contains("x-trailer"));

    auto incomplete = ncrequest::http::Http1FieldSectionParser {};
    auto partial = incomplete.push(rstd::str_::as_bytes("X-Trailer: value\r\n"));
    ASSERT_TRUE(partial.is_ok());
    EXPECT_TRUE(partial.unwrap().is_NeedMore());
    auto ended = incomplete.finish();
    ASSERT_TRUE(ended.is_err());
    EXPECT_TRUE(ended.unwrap_err().kind().is_UnexpectedEof());
}

TEST(http, MessageHeadParsesRequestTargetFormsAndTraits) {
    struct Example {
        const char* line;
        const char* method;
        const char* target;
    };
    constexpr Example examples[] = {
        { "GET /path?x=1 HTTP/1.1\r\n\r\n", "GET", "/path?x=1" },
        { "OPTIONS * HTTP/1.1\r\n\r\n", "OPTIONS", "*" },
        { "CONNECT example.com:443 HTTP/1.1\r\n\r\n", "CONNECT", "example.com:443" },
        { "GET http://example.com/path HTTP/1.1\r\n\r\n", "GET",
          "http://example.com/path" },
    };

    for (auto const& example : examples) {
        auto parsed = ncrequest::http::MessageHead::parse(
            rstd::str_::as_bytes(rstd::ref<rstd::str>(example.line)));
        ASSERT_TRUE(parsed.is_ok()) << example.line;
        auto head = rstd::move(parsed).unwrap();
        ASSERT_TRUE(head.start().is_Request()) << example.line;
        auto const& request = head.start().as_Request().value;
        EXPECT_EQ(rstd::cppstd::as_string_view(request.method().as_ref()), example.method);
        EXPECT_EQ(rstd::cppstd::as_string_view(request.target()), example.target);
        EXPECT_EQ(request.version().major(), 1u);
        EXPECT_EQ(request.version().minor(), 1u);
    }

    auto method = rstd::from_str<ncrequest::http::Method>("PATCH");
    ASSERT_TRUE(method.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", method.unwrap())), "PATCH");
    auto version = rstd::from_str<ncrequest::http::Version>("HTTP/2.0");
    ASSERT_TRUE(version.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", version.unwrap())), "HTTP/2.0");
    auto status = rstd::from_str<ncrequest::http::StatusCode>("418");
    ASSERT_TRUE(status.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", status.unwrap())), "418");
}

TEST(http, MessageHeadReportsStartAndFieldErrors) {
    auto invalid_status = ncrequest::http::MessageHead::parse(
        rstd::str_::as_bytes("HTTP/1.1 099 Bad\r\n\r\n"));
    ASSERT_TRUE(invalid_status.is_err());
    EXPECT_TRUE(invalid_status.unwrap_err().kind().is_InvalidStartLine());
    EXPECT_EQ(invalid_status.unwrap_err().offset(), 9u);

    auto bad_name_text = std::string { "HTTP/1.1 200 OK\r\nBad Name: value\r\n\r\n" };
    auto bad_name = ncrequest::http::MessageHead::parse(rstd::slice<rstd::u8>::from_raw_parts(
        reinterpret_cast<const rstd::u8*>(bad_name_text.data()), bad_name_text.size()));
    ASSERT_TRUE(bad_name.is_err());
    EXPECT_TRUE(bad_name.unwrap_err().kind().is_InvalidHeaderLine());
    EXPECT_EQ(bad_name.unwrap_err().offset(), bad_name_text.find("Bad Name") + 3);

    auto obs_fold_text = std::string { "HTTP/1.1 200 OK\r\nX: value\r\n continuation\r\n\r\n" };
    auto obs_fold = ncrequest::http::MessageHead::parse(rstd::slice<rstd::u8>::from_raw_parts(
        reinterpret_cast<const rstd::u8*>(obs_fold_text.data()), obs_fold_text.size()));
    ASSERT_TRUE(obs_fold.is_err());
    EXPECT_TRUE(obs_fold.unwrap_err().kind().is_InvalidHeaderLine());
    EXPECT_EQ(obs_fold.unwrap_err().offset(), obs_fold_text.find(" continuation"));

    auto bare_cr_text = std::string { "HTTP/1.1 200 OK\r\nX: safe\rbad\r\n\r\n" };
    auto bare_cr = ncrequest::http::MessageHead::parse(rstd::slice<rstd::u8>::from_raw_parts(
        reinterpret_cast<const rstd::u8*>(bare_cr_text.data()), bare_cr_text.size()));
    ASSERT_TRUE(bare_cr.is_err());
    EXPECT_TRUE(bare_cr.unwrap_err().kind().is_InvalidHeaderLine());
    EXPECT_EQ(bare_cr.unwrap_err().offset(), bare_cr_text.find("\rbad"));
}

TEST(http, HttpErrorDisplayTraitsDescribeStableKinds) {
    using namespace ncrequest::http;

    auto url = UrlError { UrlErrorKind::UnsupportedScheme(), 4 };
    auto header = HeaderError { HeaderErrorKind::InvalidLineBreak(), 7 };
    auto message = HttpParseError { HttpParseErrorKind::HeaderTooLarge(), 9 };
    auto query = QueryError { QueryErrorKind::InvalidUtf8(), 2 };
    auto cookie = CookieError { CookieErrorKind::InvalidAttribute(), 5 };

    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", url)),
              "HTTP URL has an unsupported scheme");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", header)),
              "line break in HTTP field value");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", message)),
              "HTTP field section is too large");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", query)),
              "invalid UTF-8 in query");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", cookie)),
              "invalid cookie attribute");
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

    auto unsupported = ncrequest::Error::Unsupported("unsupported capability");
    EXPECT_EQ(unsupported.kind(), ncrequest::ErrorKind::Unsupported);
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", unsupported)),
              "unsupported capability");
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

TEST(http, LocalHttpRedirectUsesFinalMessageHead) {
    auto base = local_http_base_url();
    if (base.empty()) {
        GTEST_SKIP() << "NCREQUEST_TEST_HTTP_BASE_URL is not set";
    }

    auto result = run_http([url = local_http_url(base, "/redirect")](auto session) {
        return fetch_text(session, url);
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

    auto request_result = run_http([url = local_http_url(base, "/headers/request-repeat")](
                                       auto session) {
        auto request = make_request(url);
        auto headers = ncrequest::http::Header {};
        (void)headers.add("X-Ncrequest-Repeat", "one");
        (void)headers.add("X-Ncrequest-Repeat", "two");
        request.update_header(headers);
        return fetch_text_request(session, rstd::move(request));
    });
#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
    ASSERT_TRUE(request_result.got_error) << request_result.error;
    EXPECT_EQ(request_result.error_kind, ncrequest::ErrorKind::Unsupported);
#else
    ASSERT_TRUE(request_result.got_body) << request_result.error;
    EXPECT_EQ(request_result.body, "one|two\n");
#endif

    auto response_result = run_http([url = local_http_url(base, "/headers/response-repeat")](
                                        auto session) {
        return fetch_text(session, url);
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
        return fetch_text(session, url);
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
