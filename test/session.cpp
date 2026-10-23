#include <rstd/test/gtest.hpp>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace h  = lifetchto::http;
using rstd::time::Duration;
namespace
{
struct Counts {
    usize pulls {}, releases {}, finished {}, stops {};
};
struct Source {
    Arc<rstd::sync::Mutex<Counts>> counts;
    auto                           next() -> coro<lf::Result<Option<Bytes>>> {
        ++counts->lock().unwrap()->pulls;
        co_return Ok(None());
    }
    void request_stop(lf::StopReason) { ++counts->lock().unwrap()->stops; }
    auto release(lf::ReleaseContext) -> coro<lf::Result<empty>> {
        ++counts->lock().unwrap()->releases;
        co_await rstd::async::sleep(Duration::from_millis(u64(20)));
        ++counts->lock().unwrap()->finished;
        co_return Ok(empty {});
    }
};
} // namespace
TEST(session, RequestMethodAndOwnedBody) {
    auto request = Request::from_url("http://example.test/path"_str).unwrap();
    EXPECT_TRUE(request.try_set_method("PROPFIND"_str).is_ok());
    EXPECT_EQ(request.method().as_ref(), "PROPFIND"_str);
    EXPECT_TRUE(request.try_set_header("X-Empty"_str, ""_str).is_ok());
    EXPECT_TRUE(request.header("x-empty"_str).is_some());
    EXPECT_TRUE(Request::from_url("bad url"_str).is_err());
    EXPECT_TRUE(request.try_set_method("bad method"_str).is_err());
    EXPECT_TRUE(request.try_set_header("bad field"_str, "x"_str).is_err());
    request.text("hello"_str).unwrap();
    EXPECT_EQ(request.body().length(), Some(u64(5)));
    EXPECT_TRUE(request.body().replayable());
}
TEST(session, RejectedRequestRetainsSourceAndOptionsAcrossCanceledRelease) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto session = Session::make(runtime.handle()).unwrap();
        EXPECT_TRUE((co_await session->close()).is_ok());
        auto counts  = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
        auto request = Request::from_url("http://example.test/rejected"_str).unwrap();
        request.try_set_method("POST"_str).unwrap();
        request.options().collect_bytes           = Some(usize(7));
        request.options().execution.partition     = String::make("user"_str);
        request.options().execution.total_timeout = Some(h::TotalTimeout::disabled());
        request.set_body(RequestBody::once(UploadBody::from_source(Source { counts.clone() })));
        auto sent = session->send(rstd::move(request));
        EXPECT_TRUE(sent.is_err());
        if (sent.is_ok()) co_return empty {};
        auto rejected = rstd::move(sent).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Closed());
        EXPECT_EQ(rejected.input.method().as_ref(), "POST"_str);
        EXPECT_EQ(rejected.input.options().collect_bytes, Some(usize(7)));
        EXPECT_EQ(rejected.input.options().execution.partition.as_str(), "user"_str);
        EXPECT_TRUE(rejected.input.options().execution.total_timeout->duration().is_none());
        EXPECT_EQ(counts->lock().unwrap()->pulls, usize());
        EXPECT_EQ(counts->lock().unwrap()->releases, usize());
        auto canceled = co_await rstd::async::timeout(rejected.input.close().into_future(),
                                                      Duration::from_millis(u64(1)));
        EXPECT_TRUE(canceled.is_err());
        EXPECT_EQ(counts->lock().unwrap()->releases, usize(1));
        EXPECT_EQ(counts->lock().unwrap()->finished, usize());
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
        EXPECT_TRUE((co_await rejected.input.close()).is_ok());
        EXPECT_EQ(counts->lock().unwrap()->releases, usize(1));
        EXPECT_EQ(counts->lock().unwrap()->finished, usize(1));
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(session, ControlBeforeSendDoesNotPullOrConsumeRequest) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto session = Session::make(runtime.handle()).unwrap();
    auto control = Arc<RequestControl>::make();
    control->cancel();
    auto sent =
        session->send(Request::from_url("http://127.0.0.1:1/"_str).unwrap(), Some(control.clone()));
    ASSERT_TRUE(sent.is_err());
    EXPECT_TRUE(sent.unwrap_err().error.is_Canceled());
    EXPECT_TRUE(runtime.block_on(session->close()).is_ok());
}
TEST(session, CloseReportPreservesErrorsAndTotal) {
    lf::CloseReport left, right;
    for (int i = 0; i < 12; ++i) left.add(lf::Error::Closed());
    for (int i = 0; i < 12; ++i) right.add(lf::Error::Canceled());
    left.merge(rstd::move(right));
    EXPECT_EQ(left.errors.len(), usize(16));
    EXPECT_EQ(left.total_errors, usize(24));
    EXPECT_EQ(left.clone().total_errors, usize(24));
}

#include "support.hpp"
namespace
{
struct Gate {
    auto run(lf::TransferContext context, lf::OutputSender<empty>) -> coro<lf::ProtocolCompletion> {
        co_await context.cancellation().stopped();
        co_return lf::ProtocolCompletion { Err(lf::Error::Canceled()),
                                           lf::RemoteOutcome::NotDispatched };
    }
    void request_stop(lf::StopReason) const {}
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> { co_return Ok(empty {}); }
};
} // namespace
TEST(session, CapacityRejectionCanBeResubmittedWithoutOpeningSource) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        lf::EngineOptions limits;
        limits.limits.operations = usize(2);
        auto engine              = lf::Engine::make(runtime.handle(), limits).unwrap();
        auto script              = test_http::Script::make(test_http::State {});
        auto session = Session::from_backend(engine.handle(),
                                             h::BackendBinding::Shared(test_http::backend(script)))
                           .unwrap();
        auto first   = engine.submit(lf::PreparedOperation<empty>::make(Gate {})).unwrap();
        auto second  = engine.submit(lf::PreparedOperation<empty>::make(Gate {})).unwrap();
        auto counts  = Arc<rstd::sync::Mutex<Counts>>::make(Counts {});
        auto input   = test_http::request();
        input.set_body(RequestBody::once(UploadBody::from_source(Source { counts.clone() })));
        auto rejected_result = session->send(rstd::move(input));
        EXPECT_TRUE(rejected_result.is_err());
        if (rejected_result.is_ok()) co_return empty {};
        auto rejected = rstd::move(rejected_result).unwrap_err();
        EXPECT_TRUE(rejected.error.is_Capacity());
        EXPECT_EQ(counts->lock().unwrap()->pulls, usize());
        (void)co_await first.control().close();
        (void)co_await second.control().close();
        auto pending = session->send(rstd::move(rejected.input)).unwrap();
        auto value   = (co_await pending.response()).unwrap();
        EXPECT_TRUE((co_await value->bytes()).is_ok());
        EXPECT_EQ(counts->lock().unwrap()->pulls, usize(1));
        EXPECT_EQ(counts->lock().unwrap()->releases, usize(1));
        EXPECT_EQ(counts->lock().unwrap()->finished, usize(1));
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(session, IndependentClientsShareBackendWithoutSharingClose) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto script  = test_http::Script::make(test_http::State {});
        auto backend = test_http::backend(script);
        auto binding = h::BackendBinding::Shared(backend.clone());
        auto first   = Session::from_backend(engine.handle(), binding).unwrap();
        auto second  = Session::from_backend(engine.handle(), binding).unwrap();
        auto alias   = first.clone();
        EXPECT_TRUE((co_await alias->close()).is_ok());
        auto rejected = first->send(test_http::request());
        EXPECT_TRUE(rejected.is_err());
        if (rejected.is_err()) EXPECT_TRUE((co_await rejected.unwrap_err().input.close()).is_ok());
        auto value = (co_await test_http::response(*second, test_http::request())).unwrap();
        EXPECT_TRUE((co_await value->bytes()).is_ok());
        EXPECT_FALSE(script->lock().unwrap()->closed);
        EXPECT_EQ(script->lock().unwrap()->backend_closes, usize());
        EXPECT_TRUE((co_await second->close()).is_ok());
        EXPECT_TRUE((co_await backend->close()).is_ok());
        EXPECT_EQ(script->lock().unwrap()->backend_closes, usize(1));
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(session, PendingOwnerAndCloseWaitCanBeDroppedWithoutLosingCleanup) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto engine                            = lf::Engine::make(runtime.handle()).unwrap();
        auto script                            = test_http::Script::make(test_http::State {});
        script->lock().unwrap()->release_delay = Duration::from_millis(u64(25));
        test_http::Reply reply;
        reply.head_delay = Duration::from_millis(u64(50));
        script->lock().unwrap()->replies.push(rstd::move(reply));
        auto session = Session::from_backend(engine.handle(),
                                             h::BackendBinding::Shared(test_http::backend(script)))
                           .unwrap();
        Option<lf::TransferControl> control;
        {
            auto pending = session->send(test_http::request()).unwrap();
            control      = Some(pending.completion());
            auto future  = pending.response();
        }
        EXPECT_TRUE((co_await control->wait())->logical.result.is_err());
        auto value   = (co_await test_http::response(*session, test_http::request())).unwrap();
        auto report  = value->completion();
        auto timeout = co_await rstd::async::timeout(session->close().into_future(),
                                                     Duration::from_millis(u64(1)));
        EXPECT_TRUE(timeout.is_err());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await report.wait())->logical.result.is_err());
        EXPECT_EQ(script->lock().unwrap()->requests.len(), usize(1));
        EXPECT_EQ(script->lock().unwrap()->releases, usize(1));
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}

namespace
{
struct FailingStore {
    Arc<rstd::dyn<h::CacheStore>> inner;
    Arc<rstd::sync::Mutex<usize>> closes;
    auto                          lookup(h::CacheKey key, lf::TransferContext context) const {
        return inner->lookup(rstd::move(key), rstd::move(context));
    }
    auto begin_write(h::CacheWriteSpec spec, lf::TransferContext context) const {
        return inner->begin_write(rstd::move(spec), rstd::move(context));
    }
    auto update(u64 id, u64 generation, h::CacheMetadata metadata,
                lf::TransferContext context) const {
        return inner->update(id, generation, rstd::move(metadata), rstd::move(context));
    }
    auto invalidate(h::CacheTarget target, lf::TransferContext context) const {
        return inner->invalidate(rstd::move(target), rstd::move(context));
    }
    void request_close() const { inner->request_close(); }
    auto close() const -> coro<lf::Result<empty>> {
        ++*closes->lock().unwrap();
        (void)co_await inner->close();
        co_await rstd::async::sleep(Duration::from_millis(u64(10)));
        co_return Err(lf::Error::BodySource("store close failed"_Str));
    }
};
} // namespace
namespace rstd
{
template<>
struct Impl<h::CacheStore, FailingStore> : ImplBase<FailingStore> {
    auto lookup(h::CacheKey key, lf::TransferContext context) const {
        return this->self().lookup(rstd::move(key), rstd::move(context));
    }
    auto begin_write(h::CacheWriteSpec spec, lf::TransferContext context) const {
        return this->self().begin_write(rstd::move(spec), rstd::move(context));
    }
    auto update(u64 id, u64 generation, h::CacheMetadata metadata,
                lf::TransferContext context) const {
        return this->self().update(id, generation, rstd::move(metadata), rstd::move(context));
    }
    auto invalidate(h::CacheTarget target, lf::TransferContext context) const {
        return this->self().invalidate(rstd::move(target), rstd::move(context));
    }
    void request_close() const { this->self().request_close(); }
    auto close() const { return this->self().close(); }
};
} // namespace rstd
TEST(session, FailedFactoryRetainsBindingsAndClosePreservesEveryError) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto engine                         = lf::Engine::make(runtime.handle()).unwrap();
        auto cache                          = h::MemoryCache::make(engine.resources()).unwrap();
        auto script                         = test_http::Script::make(test_http::State {});
        script->lock().unwrap()->fail_close = true;
        auto           binding              = h::BackendBinding::Owned(test_http::backend(script));
        auto           closes               = Arc<rstd::sync::Mutex<usize>>::make(usize());
        SessionOptions options;
        options.http.cache         = Some(h::StoreBinding::Owned(
            Arc<rstd::dyn<h::CacheStore>>::make(FailingStore { cache.store(), closes.clone() })));
        options.http.total_timeout = Some(Duration {});
        EXPECT_TRUE(Session::from_backend(engine.handle(), binding, options).is_err());
        EXPECT_TRUE(Session::make(runtime.handle(), {}, options).is_err());
        EXPECT_FALSE(script->lock().unwrap()->closed);
        EXPECT_EQ(*closes->lock().unwrap(), usize());
        options.http.total_timeout = None();
        auto session     = Session::from_backend(engine.handle(), binding, options).unwrap();
        auto interrupted = co_await rstd::async::timeout(session->close().into_future(),
                                                         Duration::from_millis(u64(1)));
        EXPECT_TRUE(interrupted.is_err());
        auto report = co_await session->close();
        EXPECT_EQ(report.total_errors, usize(2));
        EXPECT_EQ(report.errors.len(), usize(2));
        if (report.errors.len() == usize(2)) {
            EXPECT_TRUE(report.errors[usize()].is_Extension());
            EXPECT_TRUE(report.errors[usize(1)].is_BodySource());
        }
        auto repeated = co_await session->close();
        EXPECT_EQ(repeated.total_errors, report.total_errors);
        EXPECT_EQ(script->lock().unwrap()->backend_closes, usize(1));
        EXPECT_EQ(*closes->lock().unwrap(), usize(1));
        EXPECT_TRUE((co_await engine.close()).is_err());
        co_return empty {};
    };
    runtime.block_on(task());
}
