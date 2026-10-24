#include <rstd/test/gtest.hpp>
#include <stdlib.h>
#include <stdio.h>
#if LITO_FEAT_QT
#    include <QCoreApplication>
#    include <QThread>
#    include <QNetworkAccessManager>
#    include <QNetworkReply>
#    include <QNetworkProxy>
#    include <QEventLoop>
#endif
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace h  = lifetchto::http;
namespace
{
struct Counts {
    usize pulls {}, released {}, finished {};
};
struct Source {
    Arc<rstd::sync::Mutex<Counts>> counts;
    bool                           sent {};
    bool                           fail {};
    auto                           next() -> coro<lf::Result<Option<Bytes>>> {
        ++counts->lock().unwrap()->pulls;
        if (fail) co_return Err(lf::Error::InvalidOptions("source failure"_Str));
        if (rstd::exchange(sent, true)) co_return Ok(None());
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(5)));
        co_return Ok(Some(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        ++counts->lock().unwrap()->released;
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(15)));
        ++counts->lock().unwrap()->finished;
        co_return Ok(empty {});
    }
};
struct Factory {
    Arc<rstd::sync::Mutex<usize>> opens;
    auto                          open(lf::TransferContext) const -> coro<lf::Result<UploadBody>> {
        ++*opens->lock().unwrap();
        co_return Ok(UploadBody::bytes(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> { co_return Ok(empty {}); }
};
} // namespace
namespace rstd
{
template<>
struct Impl<lf::UploadFactory, Factory> : ImplBase<Factory> {
    auto open(lf::TransferContext context) const { return this->self().open(rstd::move(context)); }
    void request_stop(lf::StopReason reason) const { this->self().request_stop(reason); }
    auto release(lf::ReleaseContext context) const { return this->self().release(context); }
};
} // namespace rstd
namespace
{
auto input(ref<str> path) -> Request {
    return Request::from_url(
               rstd::format(
                   "http://127.0.0.1:{}{}",
                   rstd::ffi::CStr::from_ptr(::getenv("LIFETCHTO_PEER_PORT")).to_str().unwrap(),
                   path)
                   .as_str())
        .unwrap();
}
template<class Backend>
auto exercise(rstd::async::RuntimeHandle runtime) -> coro<empty> {
    auto engine  = lf::Engine::make(runtime.clone()).unwrap();
    auto backend = Backend::make(engine.resources()).unwrap();
    auto binding = [&] {
        if constexpr (requires { backend.shared_binding(); })
            return backend.shared_binding();
        else
            return ncrequest::BackendBinding::Shared(backend.backend());
    }();
    auto session = Session::from_backend(engine.handle(), binding).unwrap();
    EXPECT_TRUE(session->websocket_capabilities().supported);
    if (! session->capabilities().duplex) {
        auto target   = input("/must-not-dispatch-duplex"_str).url_info().clone();
        auto rejected = session->duplex({ target.clone(), {} }).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Unsupported());
        EXPECT_EQ(rejected.input.url.as_ref(), target.as_ref());
    }
    auto get      = session->get(input("/chunked"_str)).unwrap();
    auto received = co_await get.response();
    if (received.is_err()) {
        auto diagnostic = rstd::format("{}", received.unwrap_err());
        ::fprintf(stderr,
                  "%.*s\n",
                  int(diagnostic.len().to_primitive()),
                  reinterpret_cast<const char*>(diagnostic.as_str().data()));
    }
    auto response = rstd::move(received).unwrap();
    auto body     = response->take_body().unwrap();
    response.reset();
    EXPECT_EQ((co_await body.text()).unwrap().as_str(), "abcde"_str);
    auto trailers = body.trailers();
    if (session->capabilities().response_trailers)
        EXPECT_EQ(*trailers.unwrap()->get_unique_text("x-end"_str).unwrap(), "yes"_str);
    else
        EXPECT_TRUE(trailers.unwrap_err().as_Transport().cause.is_Unsupported());
    auto counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
    auto upload = input("/echo"_str);
    upload.try_set_method("PROPFIND"_str).unwrap();
    upload
        .try_set_header("X-Repeated-Fields"_str,
                        session->capabilities().repeated_request_fields ? "yes"_str : "no"_str)
        .unwrap();
    if (session->capabilities().repeated_request_fields) {
        upload.add_header("X-Dup"_str, "a"_str.as_bytes()).unwrap();
        upload.add_header("X-Dup"_str, "b"_str.as_bytes()).unwrap();
    }
    array<u8, 1> raw { u8(255) };
    upload.try_set_header("X-Bin"_str, raw.as_slice()).unwrap();
    upload.set_body(
        RequestBody::once(UploadBody::from_source(Source { counts.clone() }, Some(u64(3)))));
    auto submitted = session->send(rstd::move(upload)).unwrap();
    auto posted    = (co_await submitted.response()).unwrap();
    EXPECT_EQ(posted->header().get_all("x-dup"_str).len(), usize(2));
    EXPECT_EQ((co_await posted->text()).unwrap().as_str(), "abc"_str);
    EXPECT_EQ(counts->lock().unwrap()->released, usize(1));
    EXPECT_EQ(counts->lock().unwrap()->finished, usize(1));
    if (! session->capabilities().repeated_request_fields) {
        auto rejected_counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
        auto repeated        = input("/adapter-rejected"_str);
        repeated.add_header("X-Dup"_str, "a"_str.as_bytes()).unwrap();
        repeated.add_header("x-dup"_str, "b"_str.as_bytes()).unwrap();
        repeated.set_body(RequestBody::once(
            UploadBody::from_source(Source { rejected_counts.clone() }, Some(u64(3)))));
        auto rejected = session->send(rstd::move(repeated)).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Unsupported());
        EXPECT_EQ(rejected_counts->lock().unwrap()->pulls, usize());
        EXPECT_EQ(rejected.input.parts().headers.get_all("x-dup"_str).len(), usize(2));
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
        EXPECT_EQ(rejected_counts->lock().unwrap()->finished, usize(1));
    }
    auto declined = session->get(input("/error"_str)).unwrap();
    auto failure  = (co_await declined.response()).unwrap();
    EXPECT_EQ(failure->head().status.value(), u16(503));
    EXPECT_EQ((co_await failure->text()).unwrap().as_str(), "failed"_str);
    for (auto path : array<ref<str>, 3> { "/informational"_str, "/eof"_str, "/204"_str }) {
        auto pending = session->get(input(path)).unwrap();
        auto reply   = (co_await pending.response()).unwrap();
        EXPECT_EQ((co_await reply->text()).unwrap().as_str(),
                  path == "/eof"_str   ? "eof-body"_str
                  : path == "/204"_str ? ""_str
                                       : "hello"_str);
    }
    auto closing  = session->get(input("/close-buffered"_str)).unwrap();
    auto buffered = (co_await closing.response()).unwrap();
    co_await rstd::async::sleep(lf::Duration::from_millis(u64(20)));
    EXPECT_EQ((co_await buffered->text()).unwrap().as_str(), "hello"_str);
    auto head = input("/ok"_str);
    head.try_set_method("HEAD"_str).unwrap();
    auto headed = session->send(rstd::move(head)).unwrap();
    EXPECT_EQ((co_await (co_await headed.response()).unwrap()->bytes()).unwrap().len(), usize());
    auto large          = session->get(input("/large"_str)).unwrap();
    auto large_response = (co_await large.response()).unwrap();
    co_await rstd::async::sleep(lf::Duration::from_millis(u64(20)));
    EXPECT_EQ((co_await large_response->bytes()).unwrap().len(), usize(512 * 1024));
    auto limited = session->get(input("/large"_str)).unwrap();
    EXPECT_TRUE((co_await (co_await limited.response()).unwrap()->bytes(usize(32)))
                    .unwrap_err()
                    .is_TooLarge());
    auto truncated = session->get(input("/truncated"_str)).unwrap();
    EXPECT_TRUE((co_await (co_await truncated.response()).unwrap()->bytes()).is_err());
    auto unknown_counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
    auto unknown        = input("/echo"_str);
    unknown.try_set_method("POST"_str).unwrap();
    unknown.set_body(RequestBody::once(UploadBody::from_source(Source { unknown_counts.clone() })));
    auto unknown_sent = session->send(rstd::move(unknown));
    if (session->capabilities().unknown_upload_length) {
        auto pending = rstd::move(unknown_sent).unwrap();
        EXPECT_EQ((co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                  "abc"_str);
    } else {
        auto rejected = rstd::move(unknown_sent).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Unsupported());
        EXPECT_EQ(unknown_counts->lock().unwrap()->pulls, usize());
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
    }
    for (auto length : array<u64, 2> { u64(2), u64(4) }) {
        auto mismatch_counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
        auto mismatch        = input("/echo"_str);
        mismatch.try_set_method("POST"_str).unwrap();
        mismatch.set_body(RequestBody::once(
            UploadBody::from_source(Source { mismatch_counts.clone() }, Some(length))));
        auto pending = session->send(rstd::move(mismatch)).unwrap();
        EXPECT_TRUE((co_await pending.response()).is_err());
        EXPECT_TRUE((co_await pending.completion().wait())->logical.result.is_err());
        EXPECT_EQ(mismatch_counts->lock().unwrap()->finished, usize(1));
    }
    auto failed_counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
    auto failed_upload = input("/echo"_str);
    failed_upload.try_set_method("POST"_str).unwrap();
    failed_upload.set_body(RequestBody::once(
        UploadBody::from_source(Source { failed_counts.clone(), false, true }, Some(u64(3)))));
    auto failed_pending = session->send(rstd::move(failed_upload)).unwrap();
    auto source_error   = co_await failed_pending.response();
    EXPECT_TRUE(source_error.unwrap_err().as_Transport().cause.is_InvalidOptions());
    EXPECT_EQ(failed_counts->lock().unwrap()->finished, usize(1));
    auto early_counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
    auto early        = input("/expect-final"_str);
    early.try_set_method("POST"_str).unwrap();
    early.try_set_header("Expect"_str, "100-continue"_str).unwrap();
    early.set_body(
        RequestBody::once(UploadBody::from_source(Source { early_counts.clone() }, Some(u64(3)))));
    auto early_sent = session->send(rstd::move(early));
    if (session->capabilities().expect_continue) {
        auto pending = rstd::move(early_sent).unwrap();
        auto final   = (co_await pending.response()).unwrap();
        EXPECT_EQ(final->head().status.value(), u16(413));
        EXPECT_TRUE((co_await final->bytes()).is_ok());
    } else {
        auto rejected = rstd::move(early_sent).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Unsupported());
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
    }
    EXPECT_EQ(early_counts->lock().unwrap()->pulls, usize());
    EXPECT_EQ(early_counts->lock().unwrap()->finished, usize(1));
    SessionOptions policies;
    policies.http.policy.redirect = Some(h::RedirectPolicy {});
    h::RetryPolicy retry;
    retry.methods.push("GET"_Str);
    retry.statuses.push(u16(503));
    retry.backoff              = lf::Duration {};
    policies.http.policy.retry = Some(rstd::move(retry));
    auto policy_session        = Session::from_backend(engine.handle(), binding, policies).unwrap();
    auto retry_pending         = policy_session->get(input("/adapter-retry"_str)).unwrap();
    auto retried               = (co_await retry_pending.response()).unwrap();
    EXPECT_EQ((co_await retried->text()).unwrap().as_str(), "hello"_str);
    EXPECT_EQ((co_await retried->execution().wait()).unwrap().facts.attempts, usize(3));
    auto                      opens = Arc<rstd::sync::Mutex<usize>>::make(usize());
    Option<lihttpto::Headers> upload_trailers;
    if (policy_session->capabilities().upload_trailers) {
        lihttpto::Headers fields;
        fields.add("X-End"_str, "yes"_str).unwrap();
        upload_trailers = Some(rstd::move(fields));
    }
    auto replay = input("/adapter-307"_str);
    replay.try_set_method("POST"_str).unwrap();
    replay.set_body(
        RequestBody::repeatable({ Some(u64(3)) },
                                Arc<rstd::dyn<lf::UploadFactory>>::make(Factory { opens.clone() }),
                                rstd::move(upload_trailers)));
    auto replay_pending = policy_session->send(rstd::move(replay)).unwrap();
    auto replayed       = (co_await replay_pending.response()).unwrap();
    EXPECT_EQ((co_await replayed->text()).unwrap().as_str(), "abc"_str);
    EXPECT_EQ(*opens->lock().unwrap(), usize(2));
    EXPECT_TRUE((co_await policy_session->close()).is_ok());
    auto slow = input("/slow-body"_str);
    slow.options().execution.total_timeout =
        Some(h::TotalTimeout::after(lf::Duration::from_millis(u64(40))));
    auto timed      = session->send(rstd::move(slow)).unwrap();
    auto timed_body = (co_await timed.response()).unwrap();
    EXPECT_TRUE((co_await timed_body->bytes()).unwrap_err().as_Transport().cause.is_Timeout());
    auto unix_request = Request::from_url("http://unix.invalid/ok"_str).unwrap();
    unix_request.options().execution.routing = Some(h::Routing {
        .route = Some(Arc<h::Route>::make(
            h::Route::unix_socket(
                unix_request.url_info(),
                Bytes::copy_from_slice(rstd::ffi::CStr::from_ptr(::getenv("LIFETCHTO_UNIX_PATH"))
                                           .to_str()
                                           .unwrap()
                                           .as_bytes()))
                .unwrap())) });
    auto unix_sent                           = session->send(rstd::move(unix_request));
    if (session->capabilities().unix_socket) {
        auto pending = rstd::move(unix_sent).unwrap();
        EXPECT_EQ((co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                  "hello"_str);
    } else {
        auto rejected = rstd::move(unix_sent).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Unsupported());
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
    }
    auto routed =
        Request::from_url(
            rstd::format(
                "http://route.test:{}/ok",
                rstd::ffi::CStr::from_ptr(::getenv("LIFETCHTO_PEER_PORT")).to_str().unwrap())
                .as_str())
            .unwrap();
    Vec<rstd::net::SocketAddr> addresses;
    addresses.push(
        rstd::net::SocketAddr::ipv4_loopback(u16(::atoi(::getenv("LIFETCHTO_PEER_PORT")))));
    routed.options().execution.routing =
        Some(h::Routing { .route = Some(Arc<h::Route>::make(
                              h::Route::tcp(routed.url_info(), rstd::move(addresses)).unwrap())) });
    auto routed_sent = session->send(rstd::move(routed));
    if (session->capabilities().explicit_routes) {
        auto pending  = rstd::move(routed_sent).unwrap();
        auto received = co_await pending.response();
        if (received.is_err()) {
            auto diagnostic = rstd::format("{}", received.unwrap_err());
            ::fprintf(stderr,
                      "%.*s\n",
                      int(diagnostic.len().to_primitive()),
                      reinterpret_cast<const char*>(diagnostic.as_str().data()));
        }
        EXPECT_EQ((co_await rstd::move(received).unwrap()->text()).unwrap().as_str(), "hello"_str);
    } else {
        auto rejected = rstd::move(routed_sent).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Unsupported());
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
    }
    auto first =
        Session::from_http_backend(engine.handle(), h::BackendBinding::Shared(backend.backend()))
            .unwrap();
    EXPECT_TRUE((co_await session->close()).is_ok());
    auto survivor = first->get(input("/ok"_str)).unwrap();
    EXPECT_EQ((co_await (co_await survivor.response()).unwrap()->text()).unwrap().as_str(),
              "hello"_str);
    auto waiting  = first->get(input("/slow-body"_str)).unwrap();
    auto unread   = (co_await waiting.response()).unwrap();
    auto complete = unread->completion();
    {
        auto body = unread->take_body().unwrap();
    }
    EXPECT_TRUE((co_await complete.wait())->logical.result.is_err());
    auto closing_counts  = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
    auto closing_request = input("/stall-head"_str);
    closing_request.set_body(RequestBody::once(
        UploadBody::from_source(Source { closing_counts.clone() }, Some(u64(3)))));
    auto closing_pending = first->send(rstd::move(closing_request)).unwrap();
    {
        auto receive = closing_pending.response().into_future();
        auto poll    = co_await rstd::async::select(
            rstd::future::by_ref(receive), rstd::async::sleep(lf::Duration::from_millis(u64(1))));
        EXPECT_TRUE(poll.is_right());
    }
    {
        auto closing       = first->close().into_future();
        auto canceled_wait = co_await rstd::async::select(
            rstd::future::by_ref(closing), rstd::async::sleep(lf::Duration::from_millis(u64(1))));
        EXPECT_TRUE(canceled_wait.is_right());
    }
    EXPECT_TRUE((co_await first->close()).is_ok());
    EXPECT_EQ(closing_counts->lock().unwrap()->released, usize(1));
    EXPECT_EQ(closing_counts->lock().unwrap()->finished, usize(1));
    EXPECT_TRUE((co_await backend.close()).is_ok());
    EXPECT_EQ(engine.resources().usage().bytes, usize());
    EXPECT_TRUE((co_await engine.close()).is_ok());
    co_return empty {};
}
template<class Backend>
auto exercise_cookies(rstd::async::RuntimeHandle runtime) -> coro<empty> {
    auto           engine  = lf::Engine::make(runtime.clone()).unwrap();
    auto           backend = Backend::make(engine.resources()).unwrap();
    auto           jar     = CookieJar::make().unwrap();
    auto           cache   = h::MemoryCache::make(engine.resources()).unwrap();
    SessionOptions options;
    options.http.cookies         = Some(CookieBinding::Shared(jar.jar()));
    options.http.cache           = Some(h::StoreBinding::Shared(cache.store()));
    options.http.store_failure   = h::StoreFailure::Fail;
    options.http.policy.cache    = h::CacheMode::Default;
    options.http.policy.redirect = Some(h::RedirectPolicy {});
    h::RetryPolicy retry;
    retry.methods.push("GET"_Str);
    retry.statuses.push(u16(503));
    retry.backoff             = lf::Duration {};
    options.http.policy.retry = Some(rstd::move(retry));
    auto session              = Session::from_http_backend(
                                    engine.handle(), h::BackendBinding::Shared(backend.backend()), options)
                                    .unwrap();
    auto sibling              = Session::from_http_backend(
                                    engine.handle(), h::BackendBinding::Shared(backend.backend()), options)
                                    .unwrap();
    auto get                  = [](Arc<Session> session, Request request) -> coro<String> {
        auto pending  = session->send(rstd::move(request)).unwrap();
        auto received = co_await pending.response();
        if (received.is_err()) {
            auto diagnostic = rstd::format("{}", received.unwrap_err());
            ::fprintf(stderr,
                      "%.*s\n",
                      int(diagnostic.len().to_primitive()),
                      reinterpret_cast<const char*>(diagnostic.as_str().data()));
        }
        auto response = rstd::move(received).unwrap();
        co_return (co_await response->text()).unwrap();
    };
    EXPECT_EQ((co_await get(session.clone(), input("/cookie-start"_str))).as_str(),
              "token=two; sid=one"_str);
    EXPECT_EQ((co_await get(session.clone(), input("/cookie/retry"_str))).as_str(),
              "token=two; retry=yes; sid=one"_str);
    auto manual = input("/cookie/check"_str);
    manual.try_set_header("Cookie"_str, "manual=only"_str).unwrap();
    EXPECT_EQ((co_await get(session.clone(), rstd::move(manual))).as_str(), "manual=only"_str);
    auto disabled                        = input("/cookie/check"_str);
    disabled.options().execution.cookies = RequestCookies::Disabled();
    EXPECT_TRUE((co_await get(session.clone(), rstd::move(disabled))).is_empty());
    auto isolated = CookieJar::make().unwrap();
    auto origin   = input("/cookie/check"_str);
    EXPECT_TRUE(isolated
                    .store(origin.url_info(),
                           lihttpto::SetCookie::parse("only=other; Path=/cookie"_str).unwrap(),
                           h::cache_now())
                    .is_ok());
    origin.options().execution.cookies = RequestCookies::Shared(isolated.jar());
    EXPECT_EQ((co_await get(session.clone(), rstd::move(origin))).as_str(), "only=other"_str);
    for (int i {}; i < 2; ++i) {
        auto request                        = input("/cookie/no-store"_str);
        request.options().execution.cookies = RequestCookies::Disabled();
        auto pending                        = session->get(rstd::move(request)).unwrap();
        auto response                       = (co_await pending.response()).unwrap();
        EXPECT_FALSE(response->from_cache());
        EXPECT_TRUE((co_await response->bytes()).is_ok());
    }
    for (int i {}; i < 2; ++i) {
        auto request                        = input("/cache"_str);
        request.options().execution.cookies = RequestCookies::Disabled();
        auto pending                        = session->get(rstd::move(request)).unwrap();
        auto response                       = (co_await pending.response()).unwrap();
        EXPECT_EQ(response->from_cache(), i != 0);
        EXPECT_TRUE((co_await response->bytes()).is_ok());
    }
    for (int i {}; i < 2; ++i) {
        auto request                        = input("/adapter-chunked-cache"_str);
        request.options().execution.cookies = RequestCookies::Disabled();
        auto pending                        = session->get(rstd::move(request)).unwrap();
        auto response                       = (co_await pending.response()).unwrap();
        EXPECT_EQ(response->from_cache(), i != 0 && session->capabilities().response_trailers);
        EXPECT_EQ((co_await response->text()).unwrap().as_str(), "abcde"_str);
    }
    EXPECT_TRUE((co_await session->close()).is_ok());
    EXPECT_EQ((co_await get(sibling.clone(), input("/cookie/check"_str))).as_str(),
              "token=two; retry=yes; sid=one"_str);
    auto           owned_jar = CookieJar::make().unwrap();
    SessionOptions owner;
    owner.http.cookies = Some(CookieBinding::Owned(owned_jar.jar()));
    auto owned         = Session::from_http_backend(
                             engine.handle(), h::BackendBinding::Shared(backend.backend()), owner)
                             .unwrap();
    EXPECT_TRUE((co_await owned->close()).is_ok());
    EXPECT_TRUE(owned_jar.snapshot(h::cache_now()).unwrap_err().is_Closed());
    EXPECT_TRUE(jar.snapshot(h::cache_now()).is_ok());
    EXPECT_TRUE((co_await sibling->close()).is_ok());
    EXPECT_TRUE((co_await isolated.close()).is_ok());
    EXPECT_TRUE((co_await jar.close()).is_ok());
    EXPECT_TRUE((co_await cache.close()).is_ok());
    EXPECT_TRUE((co_await backend.close()).is_ok());
    EXPECT_TRUE((co_await engine.close()).is_ok());
    co_return empty {};
}
} // namespace
TEST(AdapterPeer, NativeUsesSharedCookiePolicy) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    runtime.block_on(exercise_cookies<h::NativeBackend>(runtime.handle()));
}
TEST(AdapterPeer, CookieFilesUseTheSharedDomainOwner) {
    auto* file = ::getenv("LIFETCHTO_COOKIE_FILE");
    if (! file) GTEST_SKIP();
    auto path = rstd::path::PathBuf::from(rstd::ffi::CStr::from_ptr(file).to_str().unwrap());
    auto jar  = CookieJar::make().unwrap();
    auto url  = lihttpto::Url::parse_http("https://cookie.test/a/b"_str).unwrap();
    auto now  = h::cache_now();
    jar.store(url,
              lihttpto::SetCookie::parse("saved=yes; Secure; HttpOnly; Path=/a"_str).unwrap(),
              now)
        .unwrap();
    EXPECT_TRUE(save_cookies(jar, path.as_path()).is_ok());
    auto loaded = CookieJar::make().unwrap();
    EXPECT_TRUE(load_cookies(loaded, path.as_path()).is_ok());
    EXPECT_TRUE(load_cookies(loaded, path.as_path()).is_ok());
    EXPECT_EQ(loaded.select(url, now).unwrap().encode().as_str(), "saved=yes"_str);
    EXPECT_EQ(loaded.snapshot(now).unwrap().len(), usize(1));
    EXPECT_TRUE(loaded.snapshot(now).unwrap()[usize()].http_only);
    EXPECT_TRUE(rstd::fs::write(path.as_path(), "bad file\n"_str.as_bytes()).is_ok());
    auto invalid = load_cookies(loaded, path.as_path());
    EXPECT_TRUE(invalid.unwrap_err().as_Transport().cause.is_Extension());
    EXPECT_EQ(loaded.select(url, now).unwrap().encode().as_str(), "saved=yes"_str);
}
TEST(AdapterPeer, NativeUsesPublicSessionAndOwnsNativeExit) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    runtime.block_on(exercise<h::NativeBackend>(runtime.handle()));
}
#if LITO_FEAT_CURL
TEST(AdapterFault, CurlFailureStopsWorkerAndRetainsCause) {
    if (! ::getenv("NCREQUEST_TEST_CURL_FAILURE") || ! ::getenv("LIFETCHTO_PEER_PORT"))
        GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = ncrequest::client::curl::Backend::make(engine.resources()).unwrap();
        auto session =
            Session::from_http_backend(engine.handle(), h::BackendBinding::Owned(backend.backend()))
                .unwrap();
        auto counts = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
        auto upload = input("/echo"_str);
        upload.try_set_method("POST"_str).unwrap();
        upload.set_body(
            RequestBody::once(UploadBody::from_source(Source { counts.clone() }, Some(u64(3)))));
        auto pending  = session->send(rstd::move(upload)).unwrap();
        auto response = co_await pending.response();
        if (response.is_ok()) EXPECT_TRUE((co_await (*response)->bytes()).is_err());
        EXPECT_TRUE((co_await pending.completion().wait())->logical.result.is_err());
        auto closed = co_await backend.close();
        EXPECT_TRUE(closed.is_err());
        if (closed.is_err()) {
            const auto& error = closed.as_ref().unwrap_err();
            EXPECT_TRUE(error.is_Extension());
            if (error.is_Extension()) {
                auto cause = rstd::error::downcast_ref<ncrequest::client::curl::Error>(
                    error.as_Extension().cause.deref());
                EXPECT_TRUE(cause.is_some());
                if (cause.is_some()) EXPECT_TRUE((*cause)->is_multi());
            }
        }
        EXPECT_TRUE((co_await session->close()).is_err());
        EXPECT_EQ(counts->lock().unwrap()->finished, usize(1));
        EXPECT_TRUE((co_await engine.close()).is_err());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(AdapterPeer, CurlUsesSharedCookiePolicy) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    runtime.block_on(exercise_cookies<ncrequest::client::curl::Backend>(runtime.handle()));
}
TEST(AdapterPeer, CurlUsesPublicSessionAndOwnsNativeExit) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    runtime.block_on(exercise<ncrequest::client::curl::Backend>(runtime.handle()));
}
#endif
#if LITO_FEAT_QT
TEST(AdapterPeer, QtUsesSharedCookiePolicy) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    runtime.block_on(exercise_cookies<ncrequest::client::qt_network::Backend>(runtime.handle()));
}
TEST(AdapterPeer, QtUsesPublicSessionAndOwnsNativeExit) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    runtime.block_on(exercise<ncrequest::client::qt_network::Backend>(runtime.handle()));
}
TEST(AdapterPeer, QtExternalManagerSurvivesBackendClose) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto    runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    QNetworkAccessManager*                         manager {};
    Option<ncrequest::client::qt_network::Backend> backend;
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            manager = new QNetworkAccessManager;
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            ncrequest::client::qt_network::Options options;
            options.manager = manager;
            backend         = Some(ncrequest::client::qt_network::Backend::make(engine.resources(),
                                                                                rstd::move(options))
                                       .unwrap());
        },
        Qt::BlockingQueuedConnection);
    auto task = [&]() -> coro<empty> {
        auto session = Session::from_http_backend(engine.handle(),
                                                  h::BackendBinding::Owned(backend->backend()))
                           .unwrap();
        auto pending = session->get(input("/ok"_str)).unwrap();
        EXPECT_EQ((co_await (co_await pending.response()).unwrap()->text()).unwrap().as_str(),
                  "hello"_str);
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
    auto request = input("/ok"_str);
    auto url     = request.url_info().as_ref();
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            EXPECT_EQ(manager->proxy().type(), QNetworkProxy::NoProxy);
            auto*      reply = manager->get(QNetworkRequest(QUrl::fromEncoded(QByteArray(
                reinterpret_cast<const char*>(url.data()), int(url.size().to_primitive())))));
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            EXPECT_EQ(reply->error(), QNetworkReply::NoError);
            EXPECT_TRUE(reply->readAll() == QByteArray("hello"));
            delete reply;
            delete manager;
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
}
TEST(AdapterPeer, QtExternalManagerDestructionCompletesPendingWork) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto    runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    QNetworkAccessManager*                         manager {};
    Option<ncrequest::client::qt_network::Backend> backend;
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            manager = new QNetworkAccessManager;
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            ncrequest::client::qt_network::Options options;
            options.manager = manager;
            backend         = Some(ncrequest::client::qt_network::Backend::make(engine.resources(),
                                                                                rstd::move(options))
                                       .unwrap());
        },
        Qt::BlockingQueuedConnection);
    auto task = [&]() -> coro<empty> {
        auto session = Session::from_http_backend(engine.handle(),
                                                  h::BackendBinding::Shared(backend->backend()))
                           .unwrap();
        auto pending = session->get(input("/stall-head"_str)).unwrap();
        {
            auto head    = pending.response().into_future();
            auto waiting = co_await rstd::async::select(
                rstd::future::by_ref(head), rstd::async::sleep(lf::Duration::from_millis(u64(30))));
            EXPECT_TRUE(waiting.is_right());
        }
        QMetaObject::invokeMethod(
            &bridge,
            [&] {
                delete manager;
            },
            Qt::BlockingQueuedConnection);
        EXPECT_TRUE((co_await pending.response()).is_err());
        EXPECT_TRUE((co_await pending.completion().wait())->logical.result.is_err());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
}
#endif

#if LITO_FEAT_QT
TEST(AdapterPeer, QtCapacityRecoveryAndProducerEofOnMultipleWorkers) {
    if (! ::getenv("LIFETCHTO_PEER_PORT")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::multi_thread()
                       .worker_threads(usize(4))
                       .enable_all()
                       .build()
                       .unwrap();
    auto test    = [&]() -> coro<empty> {
        auto                        engine = lf::Engine::make(runtime.handle()).unwrap();
        client::qt_network::Options options;
        options.window = usize(1024);
        auto backend =
            client::qt_network::Backend::make(engine.resources(), rstd::move(options)).unwrap();
        auto session = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        EXPECT_FALSE(session->capabilities().unknown_upload_length);
        {
            auto counts  = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
            auto request = input("/echo"_str);
            request.try_set_method("POST"_str).unwrap();
            request.set_body(
                RequestBody::once(UploadBody::from_source(Source { counts.clone() }, None<u64>())));
            auto rejected = session->send(rstd::move(request)).unwrap_err();
            EXPECT_TRUE(rejected.error.is_Unsupported());
            EXPECT_EQ(counts->lock().unwrap()->pulls, usize());
            EXPECT_TRUE((co_await rejected.input.close()).is_ok());
        }
        for (int i {}; i < 4; ++i) {
            auto submitted = session->get(input("/large"_str)).unwrap();
            auto response  = (co_await submitted.response()).unwrap();
            co_await rstd::async::sleep(lf::Duration::from_millis(u64(1)));
            EXPECT_EQ((co_await response->bytes()).unwrap().len(), usize(512 * 1024));
            auto counts  = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
            auto request = input("/echo"_str);
            request.try_set_method("POST"_str).unwrap();
            request.set_body(RequestBody::once(
                UploadBody::from_source(Source { counts.clone() }, Some(u64(3)))));
            auto upload = session->send(rstd::move(request)).unwrap();
            EXPECT_EQ((co_await (co_await upload.response()).unwrap()->text()).unwrap().as_str(),
                      "abc"_str);
            EXPECT_EQ(counts->lock().unwrap()->released, usize(1));
        }
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().tasks, usize());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
#endif
