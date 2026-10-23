#include <rstd/test/gtest.hpp>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
#include "support.hpp"
using namespace test_http;
namespace
{
struct Factory {
    Arc<rstd::sync::Mutex<usize>> opens;
    auto                          open(lf::TransferContext) const -> coro<lf::Result<UploadBody>> {
        ++*opens->lock().unwrap();
        co_return Ok(UploadBody::bytes(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> { co_return Ok(empty {}); }
};
struct CollectSink {
    using Error = rstd::io::Error;
    Vec<u8> bytes;
    auto    write(Bytes part) -> coro<rstd::Result<empty, Error>> {
        bytes.extend_from_slice(part.as_slice());
        co_return Ok(empty {});
    }
};
struct Sink {
    using Error = rstd::io::Error;
    auto write(Bytes) -> coro<rstd::Result<empty, Error>> {
        co_return Err(
            Error::from_kind(rstd::io::ErrorKind { rstd::io::ErrorKind::PermissionDenied }));
    }
};
} // namespace
namespace rstd
{
template<>
struct Impl<lifetchto::UploadFactory, Factory> : ImplBase<Factory> {
    auto open(lifetchto::TransferContext c) const { return this->self().open(rstd::move(c)); }
    void request_stop(lifetchto::StopReason r) const { this->self().request_stop(r); }
    auto release(lifetchto::ReleaseContext c) const { return this->self().release(c); }
};
} // namespace rstd
TEST(upload, FactoryReopensAndRetryHasOneOwner) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  script = Script::make(State {});
        Reply retry, ok;
        retry.status = u16(503);
        script->lock().unwrap()->replies.push(rstd::move(retry));
        script->lock().unwrap()->replies.push(rstd::move(ok));
        SessionOptions options;
        h::RetryPolicy policy;
        policy.methods.push("POST"_Str);
        policy.statuses.push(u16(503));
        policy.business_idempotent = true;
        policy.backoff             = Duration {};
        options.http.policy.retry  = Some(rstd::move(policy));
        auto session = Session::from_backend(engine.handle(),
                                             h::BackendBinding::Shared(backend(script)),
                                             rstd::move(options))
                           .unwrap();
        auto opens   = Arc<rstd::sync::Mutex<usize>>::make(usize());
        auto input   = request();
        input.try_set_method("POST"_str).unwrap();
        input.set_body(RequestBody::repeatable(
            { Some(u64(3)) }, Arc<rstd::dyn<lf::UploadFactory>>::make(Factory { opens.clone() })));
        auto value = (co_await response(*session, rstd::move(input))).unwrap();
        EXPECT_TRUE((co_await value->bytes()).is_ok());
        EXPECT_EQ(*opens->lock().unwrap(), usize(2));
        EXPECT_EQ(script->lock().unwrap()->prepared, usize(1));
        EXPECT_EQ(script->lock().unwrap()->requests.len(), usize(2));
        EXPECT_EQ(script->lock().unwrap()->uploads[usize()].as_slice(), "abc"_str.as_bytes());
        EXPECT_EQ(script->lock().unwrap()->uploads[usize(1)].as_slice(), "abc"_str.as_bytes());
        EXPECT_EQ((co_await value->execution().wait()).unwrap().facts.attempts, usize(2));
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(upload, OnceRedirectRefusesReplay) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  script = Script::make(State {});
        Reply reply;
        reply.status = u16(307);
        reply.headers.add("Location"_str, "/next"_str).unwrap();
        script->lock().unwrap()->replies.push(rstd::move(reply));
        SessionOptions options;
        options.http.policy.redirect = Some(h::RedirectPolicy {});
        auto session = Session::from_backend(engine.handle(),
                                             h::BackendBinding::Shared(backend(script)),
                                             rstd::move(options))
                           .unwrap();
        auto input   = request();
        input.try_set_method("POST"_str).unwrap();
        input.set_body(
            RequestBody::once(UploadBody::bytes(Bytes::copy_from_slice("abc"_str.as_bytes()))));
        auto result = co_await response(*session, rstd::move(input));
        EXPECT_TRUE(result.is_err());
        if (result.is_err())
            EXPECT_TRUE(result.unwrap_err().as_Transport().cause.is_ReplayUnavailable());
        EXPECT_EQ(script->lock().unwrap()->requests.len(), usize(1));
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(upload, TruncationAndSinkFailureKeepTypedErrorsAndStopBody) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  script = Script::make(State {});
        Reply truncated;
        truncated.truncated = true;
        script->lock().unwrap()->replies.push(rstd::move(truncated));
        auto session =
            Session::from_backend(engine.handle(), h::BackendBinding::Shared(backend(script)))
                .unwrap();
        auto first = (co_await response(*session, request())).unwrap();
        EXPECT_TRUE((co_await first->bytes()).unwrap_err().as_Transport().cause.is_BodySource());
        EXPECT_TRUE((co_await first->completion().wait())->logical.result.is_err());
        auto value   = (co_await response(*session, request())).unwrap();
        auto control = value->completion();
        Sink sink;
        auto copy = value->read_to_stream(sink);
        value.reset();
        auto failed = co_await rstd::move(copy);
        EXPECT_TRUE(failed.is_err());
        if (failed.is_err()) EXPECT_TRUE(failed.unwrap_err().is_Sink());
        EXPECT_TRUE((co_await control.wait())->logical.result.is_err());
        auto        copied = (co_await response(*session, request())).unwrap();
        CollectSink output;
        auto        copying = copied->read_to_stream(output);
        copied.reset();
        EXPECT_EQ((co_await rstd::move(copying)).unwrap(), u64(7));
        EXPECT_EQ(output.bytes.as_slice(), "payload"_str.as_bytes());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
