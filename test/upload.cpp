#include <rstd/test/gtest.hpp>
#include <cstdlib>
#include <string>
import ncrequest;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using ncrequest::Request;
using ncrequest::RequestBody;
using ncrequest::RequestOptions;
using ncrequest::Session;
using rstd::async::block_on;
using rstd::async::RuntimeBuilder;
using rstd::async::sleep;
using rstd::async::spawn_local;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::time::Duration;
using IoError     = rstd::io::error::Error;
using IoErrorKind = rstd::io::error::ErrorKind;

static auto text_ref(const std::string& text) -> ref<str> {
    return rstd::str_::from_utf8(
               slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(text.data()),
                                         usize(text.size())))
        .unwrap();
}

static auto base_url() -> std::string {
    auto value = std::getenv("NCREQUEST_TEST_HTTP_BASE_URL");
    return value ? value : "";
}

struct UploadStats {
    Atomic<int> calls { 0 };
    Atomic<int> pending { 0 };
};

struct AsyncSource {
    using Error = IoError;
    Arc<UploadStats> stats;
    usize            chunks { 3 };
    usize            chunk_size { 40000 };
    u64              delay_ms { 1 };
    bool             fail { false };
    bool             empty_first { false };
    auto             next() -> ncrequest::coro<rstd::Result<Option<Bytes>, Error>> {
        stats->calls.fetch_add(1);
        struct Guard {
            Arc<UploadStats> stats;
            explicit Guard(Arc<UploadStats> value): stats(rstd::move(value)) {
                stats->pending.fetch_add(1);
            }
            ~Guard() { stats->pending.fetch_sub(1); }
        } guard(stats.clone());
        co_await sleep(Duration::from_millis(delay_ms));
        if (fail) co_return Err(IoError::from_kind(IoErrorKind { IoErrorKind::PermissionDenied }));
        if (empty_first) {
            empty_first = false;
            co_return Ok(Some(Bytes {}));
        }
        if (chunks == usize()) co_return Ok(None<Bytes>());
        --chunks;
        auto payload = std::string(chunk_size.to_primitive(), 'x');
        if (! payload.empty()) payload[0] = '\0';
        co_return Ok(Some(Bytes::copy_from_slice(text_ref(payload).as_bytes())));
    }
};
static_assert(lihttpto::BodySource<AsyncSource>);

struct UploadResult {
    bool                     ok { false };
    int                      code {};
    std::string              body;
    Option<ncrequest::Error> error;
};

static auto upload(Arc<Session> session, std::string url, std::string method, AsyncSource source,
                   Option<u64> size, bool redirects = false) -> ncrequest::coro<UploadResult> {
    auto request = Request::from_url(text_ref(url)).unwrap();
    request.try_set_method(text_ref(method)).unwrap();
    request.set_body(RequestBody::from_source(rstd::move(source), size));
    request.try_set_header("Expect"_str, ""_str).unwrap();
    auto options     = RequestOptions {};
    options.redirect = Some(redirects ? ncrequest::RedirectOptions::same_origin()
                                      : ncrequest::RedirectOptions::disabled());
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto timeout    = ncrequest::TimeoutOptions {};
    timeout.total   = ncrequest::TimeoutLimit::after(Duration::from_secs(u64(2)));
    options.timeout = Some(timeout);
#endif
    request.set_options(rstd::move(options));
    auto response = co_await session->send(rstd::move(request));
    if (response.is_err())
        co_return UploadResult { false, 0, {}, Some(rstd::move(response).unwrap_err()) };
    auto value = rstd::move(response).unwrap();
    if (url.ends_with("/async-duplex")) co_await sleep(Duration::from_millis(u64(100)));
    auto body = co_await value->bytes();
    if (body.is_err()) co_return UploadResult { false, 0, {}, Some(rstd::move(body).unwrap_err()) };
    auto raw = body->as_slice();
    co_return UploadResult { true,
                             value->code()->to_primitive(),
                             std::string(reinterpret_cast<const char*>(raw.as_raw_ptr()),
                                         raw.len().to_primitive()),
                             None() };
}

TEST(upload, SourceOwnershipAndValidation) {
    auto request = Request::from_url("http://localhost/"_str).unwrap();
    request.set_body(RequestBody::from_source(AsyncSource { Arc<UploadStats>::make() }));
    EXPECT_TRUE(request.try_clone().is_err());
    request.try_set_method("HEAD"_str).unwrap();
    EXPECT_TRUE(request.validate().is_err());
    request.clear_body();
    EXPECT_TRUE(request.validate().is_ok());
}

TEST(upload, LocalAsyncMethodsAndLengths) {
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto base = base_url();
    if (base.empty()) GTEST_SKIP();
    for (auto method : { "POST", "PUT", "PATCH", "GET", "DELETE", "REPORT" }) {
        for (bool known : { false, true }) {
            auto source        = AsyncSource { Arc<UploadStats>::make() };
            source.empty_first = true;
            auto result        = block_on(upload(Session::make().unwrap(),
                                                 base + "/async-echo",
                                                 method,
                                                 rstd::move(source),
                                                 known ? Some(u64(120000)) : None<u64>()));
            ASSERT_TRUE(result.ok);
            EXPECT_EQ(result.code, 200);
            auto expected = std::string(method) + "\n";
            auto chunk    = std::string(40000, 'x');
            chunk[0]      = '\0';
            expected += chunk + chunk + chunk;
            EXPECT_EQ(result.body, expected);
        }
    }
    auto stats = Arc<UploadStats>::make();
    auto empty = block_on(upload(Session::make().unwrap(),
                                 base + "/async-echo",
                                 "POST",
                                 AsyncSource { stats.clone() },
                                 Some(u64())));
    EXPECT_TRUE(empty.ok);
    EXPECT_EQ(empty.body, "POST\n");
    EXPECT_EQ(stats->calls.load(), 0);

    auto no_chunks     = AsyncSource { Arc<UploadStats>::make() };
    no_chunks.chunks   = usize();
    auto unknown_empty = block_on(upload(Session::make().unwrap(),
                                         base + "/async-echo",
                                         "POST",
                                         rstd::move(no_chunks),
                                         None<u64>()));
    EXPECT_TRUE(unknown_empty.ok);
    EXPECT_EQ(unknown_empty.body, "POST\n");

    auto runtime =
        RuntimeBuilder::multi_thread().worker_threads(usize(2)).enable_time().build().unwrap();
    auto result = runtime.block_on(upload(Session::make().unwrap(),
                                          base + "/async-echo",
                                          "PATCH",
                                          AsyncSource { Arc<UploadStats>::make() },
                                          None<u64>()));
    EXPECT_TRUE(result.ok);
#else
    GTEST_SKIP();
#endif
}

TEST(upload, LocalAsyncErrorsAndRedirect) {
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto base = base_url();
    if (base.empty()) GTEST_SKIP();
    auto stats    = Arc<UploadStats>::make();
    auto overflow = block_on(upload(Session::make().unwrap(),
                                    base + "/async-echo",
                                    "POST",
                                    AsyncSource { stats.clone() },
                                    Some(u64(u64::MAX))));
    EXPECT_TRUE(overflow.error.is_some() && overflow.error->is_InvalidState());
    EXPECT_EQ(stats->calls.load(), 0);
    for (auto size : { 119999u, 120001u }) {
        auto result = block_on(upload(Session::make().unwrap(),
                                      base + "/async-echo",
                                      "POST",
                                      AsyncSource { Arc<UploadStats>::make() },
                                      Some(u64(size))));
        ASSERT_TRUE(result.error.is_some());
        ASSERT_TRUE(result.error->is_Protocol());
        EXPECT_EQ(result.error->as_Protocol().kind, ncrequest::ProtocolError::BodyLengthMismatch);
    }
    auto source = AsyncSource { Arc<UploadStats>::make() };
    source.fail = true;
    auto failed = block_on(upload(
        Session::make().unwrap(), base + "/async-echo", "POST", rstd::move(source), None<u64>()));
    ASSERT_TRUE(failed.error.is_some());
    ASSERT_TRUE(failed.error->is_Io());
    EXPECT_EQ(failed.error->as_Io().error.kind(), (IoErrorKind { IoErrorKind::PermissionDenied }));
    auto redirect = block_on(upload(Session::make().unwrap(),
                                    base + "/async-redirect",
                                    "POST",
                                    AsyncSource { Arc<UploadStats>::make() },
                                    Some(u64(120000)),
                                    true));
    ASSERT_TRUE(redirect.error.is_some());
    ASSERT_TRUE(redirect.error->is_Protocol());
    EXPECT_EQ(redirect.error->as_Protocol().kind,
              ncrequest::ProtocolError::RedirectBodyNotReplayable);
#else
    GTEST_SKIP();
#endif
}

static auto cancel_upload(std::string url, bool close_session, bool blocked_queue)
    -> ncrequest::coro<bool> {
    auto session      = Session::make().unwrap();
    auto stats        = Arc<UploadStats>::make();
    auto source       = AsyncSource { stats.clone() };
    source.delay_ms   = u64(blocked_queue ? 0 : 10000);
    source.chunks     = usize(10000);
    source.chunk_size = usize(65536);
    auto pending =
        spawn_local(upload(session.clone(), url, "POST", rstd::move(source), None<u64>()));
    co_await sleep(Duration::from_millis(u64(100)));
    auto before = stats->calls.load();
    if (close_session)
        session->close();
    else
        pending.abort();
    auto result = co_await rstd::move(pending);
    bool failed = result.is_err() || result->error.is_some();
    for (int i = 0; i < 100 && stats->pending.load() != 0; ++i)
        co_await sleep(Duration::from_millis(u64(2)));
    auto calls = stats->calls.load();
    co_await sleep(Duration::from_millis(u64(20)));
    co_return before > 0 && before < 10000 && failed && stats->pending.load() == 0 &&
        calls == stats->calls.load();
}

TEST(upload, LocalAsyncCancellation) {
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto base = base_url();
    if (base.empty()) GTEST_SKIP();
    for (bool close_session : { false, true })
        for (bool blocked_queue : { false, true })
            EXPECT_TRUE(
                block_on(cancel_upload(base + "/async-stall", close_session, blocked_queue)));
#else
    GTEST_SKIP();
#endif
}

static auto early_response(std::string url) -> ncrequest::coro<bool> {
    auto stats      = Arc<UploadStats>::make();
    auto source     = AsyncSource { stats.clone() };
    source.delay_ms = u64(10000);
    auto result =
        co_await upload(Session::make().unwrap(), url, "POST", rstd::move(source), None<u64>());
    for (int i = 0; i < 100 && stats->pending.load() != 0; ++i)
        co_await sleep(Duration::from_millis(u64(2)));
    co_return result.ok&& result.code == 413 && result.body == "rejected" &&
        stats->pending.load() == 0;
}

static auto upload_timeout(std::string url) -> ncrequest::coro<bool> {
    auto stats      = Arc<UploadStats>::make();
    auto source     = AsyncSource { stats.clone() };
    source.delay_ms = u64(10000);
    auto result =
        co_await upload(Session::make().unwrap(), url, "POST", rstd::move(source), None<u64>());
    for (int i = 0; i < 100 && stats->pending.load() != 0; ++i)
        co_await sleep(Duration::from_millis(u64(2)));
    co_return result.error.is_some() && result.error->is_Client() &&
        result.error->as_Client().error.code == i32(28) && stats->pending.load() == 0;
}

TEST(upload, LocalAsyncTimeoutAndDuplex) {
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto base = base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(block_on(upload_timeout(base + "/async-stall")));
    auto source   = AsyncSource { Arc<UploadStats>::make() };
    source.chunks = usize(30);
    auto result   = block_on(upload(
        Session::make().unwrap(), base + "/async-duplex", "POST", rstd::move(source), None<u64>()));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.body, std::string(262144, 'd'));
#else
    GTEST_SKIP();
#endif
}

TEST(upload, LocalAsyncEarlyResponse) {
#ifdef NCREQUEST_CLIENT_BACKEND_CURL
    auto base = base_url();
    if (base.empty()) GTEST_SKIP();
    EXPECT_TRUE(block_on(early_response(base + "/async-early")));
#else
    auto result = block_on(upload(Session::make().unwrap(),
                                  "http://localhost/",
                                  "POST",
                                  AsyncSource { Arc<UploadStats>::make() },
                                  None<u64>()));
    EXPECT_TRUE(result.error.is_some() && result.error->is_Unsupported());
#endif
}
