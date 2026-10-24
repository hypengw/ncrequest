#include <rstd/test/gtest.hpp>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
#include "support.hpp"
using namespace test_http;
TEST(http, OwnedSingleConsumerBodyAndTrailers) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        auto script = Script::make(State {});
        auto session =
            Session::from_http_backend(engine.handle(), h::BackendBinding::Shared(backend(script)))
                .unwrap();
        auto value   = (co_await response(*session, request())).unwrap();
        auto control = value->completion();
        EXPECT_TRUE(value->trailers().unwrap().is_none());
        auto body = value->take_body().unwrap();
        EXPECT_TRUE(value->take_body().unwrap_err().is_BodyTaken());
        value.reset();
        EXPECT_EQ((co_await body.collect()).unwrap().as_slice(), "payload"_str.as_bytes());
        EXPECT_TRUE(body.trailers().unwrap().is_some());
        EXPECT_TRUE((co_await control.wait())->logical.result.is_ok());
        EXPECT_TRUE((co_await body.collect()).unwrap_err().is_Consumed());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, DecodeCauseDoesNotChangeSuccessfulHttpReport) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto         engine = lf::Engine::make(runtime.handle()).unwrap();
        auto         script = Script::make(State {});
        Reply        bad;
        array<u8, 1> raw { u8(255) };
        bad.data = Bytes::copy_from_slice(raw.as_slice());
        script->lock().unwrap()->replies.push(rstd::move(bad));
        auto session =
            Session::from_http_backend(engine.handle(), h::BackendBinding::Shared(backend(script)))
                .unwrap();
        auto value = (co_await response(*session, request())).unwrap();
        auto error = (co_await value->text()).unwrap_err();
        EXPECT_TRUE(error.is_Decode());
        auto cause = rstd::as<rstd::error::Error>(error).source();
        EXPECT_TRUE(cause.is_some());
        if (cause.is_some()) EXPECT_TRUE(rstd::error::is<rstd::str_::Utf8Error>(*cause));
        EXPECT_TRUE((co_await value->completion().wait())->logical.result.is_ok());
        auto second = (co_await response(*session, request())).unwrap();
        EXPECT_TRUE((co_await second->json()).unwrap_err().is_Decode());
        EXPECT_TRUE((co_await second->completion().wait())->logical.result.is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, CollectionLimitCannotBeRaisedAndZeroAllowsEmptyBody) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  script = Script::make(State {});
        Reply data, empty_reply;
        empty_reply.data = Bytes {};
        script->lock().unwrap()->replies.push(rstd::move(data));
        script->lock().unwrap()->replies.push(rstd::move(empty_reply));
        auto session =
            Session::from_http_backend(engine.handle(), h::BackendBinding::Shared(backend(script)))
                .unwrap();
        auto input                    = request();
        input.options().collect_bytes = Some(usize(3));
        auto value                    = (co_await response(*session, rstd::move(input))).unwrap();
        auto result                   = co_await value->bytes(usize(999));
        EXPECT_TRUE(result.is_err());
        if (result.is_err()) {
            EXPECT_TRUE(result.unwrap_err().is_TooLarge());
            EXPECT_EQ(result.unwrap_err().as_TooLarge().limit, usize(3));
        }
        auto empty_input                    = request();
        empty_input.options().collect_bytes = Some(usize());
        auto empty_response = (co_await response(*session, rstd::move(empty_input))).unwrap();
        EXPECT_EQ((co_await empty_response->bytes()).unwrap().len(), usize());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, TotalTimeoutOverrideKeepsExplicitDeadline) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        for (int mode = 0; mode < 5; ++mode) {
            auto  engine = lf::Engine::make(runtime.handle()).unwrap();
            auto  script = Script::make(State {});
            Reply reply;
            reply.head_delay = Duration::from_millis(u64(30));
            script->lock().unwrap()->replies.push(rstd::move(reply));
            SessionOptions config;
            config.http.total_timeout = Some(Duration::from_millis(u64(5)));
            auto session =
                Session::from_http_backend(engine.handle(),
                                           h::BackendBinding::Shared(backend(script.clone())),
                                           rstd::move(config))
                    .unwrap();
            auto input = request();
            if (mode == 1 || mode == 3)
                input.options().execution.total_timeout = Some(h::TotalTimeout::disabled());
            if (mode == 2)
                input.options().execution.total_timeout =
                    Some(h::TotalTimeout::after(Duration::from_secs(u64(1))));
            if (mode == 3)
                input.options().execution.transfer.deadline =
                    Some(lf::Instant::now() + Duration::from_millis(u64(5)));
            if (mode == 4)
                input.options().execution.total_timeout =
                    Some(h::TotalTimeout::after(Duration::from_millis(u64(1))));
            auto result = co_await response(*session, rstd::move(input));
            if (mode == 1 || mode == 2) {
                EXPECT_TRUE(result.is_ok());
                if (result.is_ok()) EXPECT_TRUE((co_await (*result)->bytes()).is_ok());
            } else {
                EXPECT_TRUE(result.is_err());
                if (result.is_err())
                    EXPECT_TRUE(result.unwrap_err().as_Transport().cause.is_Timeout());
            }
            if (mode == 1) EXPECT_TRUE(script->lock().unwrap()->deadline.is_none());
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_TRUE((co_await engine.close()).is_ok());
        }
        co_return empty {};
    };
    runtime.block_on(task());
}
TEST(http, ConcurrentCollectionAndCanceledWaitKeepTheirOwners) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto task    = [&]() -> coro<empty> {
        auto  engine = lf::Engine::make(runtime.handle()).unwrap();
        auto  script = Script::make(State {});
        Reply reply;
        reply.head_delay = Duration::from_millis(u64(15));
        reply.body_delay = Duration::from_millis(u64(25));
        script->lock().unwrap()->replies.push(rstd::move(reply));
        auto session =
            Session::from_http_backend(engine.handle(), h::BackendBinding::Shared(backend(script)))
                .unwrap();
        auto pending = session->send(request()).unwrap();
        auto timed   = co_await rstd::async::timeout(pending.response().into_future(),
                                                     Duration::from_millis(u64(1)));
        EXPECT_TRUE(timed.is_err());
        auto value = (co_await pending.response()).unwrap();
        auto body  = value->take_body().unwrap();
        auto first = rstd::async::spawn(body.collect());
        co_await rstd::async::sleep(Duration::from_millis(u64(1)));
        EXPECT_TRUE((co_await body.collect()).unwrap_err().is_Busy());
        auto read = co_await first;
        EXPECT_TRUE(read.unwrap().is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(task());
}
