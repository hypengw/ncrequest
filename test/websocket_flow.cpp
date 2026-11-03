#include <rstd/test/gtest.hpp>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace flow_test
{
namespace lf = lifetchto;
namespace ws = liwebsocketo;
using rstd::async::Completion;
using rstd::async::CompletionHandle;
struct Fields {
    Vec<Bytes>                                                 written;
    Option<CompletionHandle<ws::WebSocketResult<empty>>>       writer;
    Option<CompletionHandle<ws::WebSocketResult<ws::Message>>> receiver;
    Option<ws::WebSocketError>                                 terminal;
    Option<lf::Error>                                          error;
    Option<lf::FailureReport>                                  diagnostic;
    bool                                                       closing {};
    usize                                                      receipts {};
};
struct State {
    rstd::sync::Mutex<Fields> fields { Fields {} };
    lf::Event                 changed;
    void                      stop(ws::WebSocketError error) {
        {
            auto f = fields.lock().unwrap();
            if (f->terminal.is_some()) return;
            f->terminal = Some(error);
            if (f->writer.is_some()) (void)f->writer.take()->complete(Err(error));
            if (f->receiver.is_some()) (void)f->receiver.take()->complete(Err(error));
        }
        changed.notify();
    }
    void finish_write() {
        {
            auto f = fields.lock().unwrap();
            if (f->writer.is_some()) (void)f->writer.take()->complete(Ok(empty {}));
        }
        changed.notify();
    }
    void deliver() {
        {
            auto f = fields.lock().unwrap();
            ++f->receipts;
            (void)f->receiver.take()->complete(Ok(
                ws::Message { ws::Kind::Text, Bytes::copy_from_slice("payload"_str.as_bytes()) }));
        }
        changed.notify();
    }
};
struct Driver {
    Arc<State> state;
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> { co_return Ok(empty {}); }
    void request_stop(lf::StopReason) const { state->stop(ws::WebSocketError::Cancelled); }
    auto run(lf::TransferContext, lf::OutputSender<empty> output) const
        -> coro<lf::ProtocolCompletion> {
        (void)output.publish(empty {});
        for (;;) {
            auto event = state->changed.listen();
            if (state->fields.lock().unwrap()->terminal.is_some()) break;
            co_await rstd::move(event);
        }
        co_return lf::ProtocolCompletion { Ok(empty {}), lf::RemoteOutcome::Unknown };
    }
};
struct Endpoint {
    Arc<State>          state;
    lf::Transfer<empty> transfer;
    auto                protocol() const -> Option<String> { return None(); }
    auto                send(ws::Message message) const -> Completion<ws::WebSocketResult<empty>> {
        auto pair = Completion<ws::WebSocketResult<empty>>::make().unwrap();
        {
            auto f = state->fields.lock().unwrap();
            EXPECT_TRUE(f->writer.is_none());
            f->written.push(rstd::move(message.data));
            f->writer = Some(rstd::move(pair.get<1>()));
        }
        state->changed.notify();
        return rstd::move(pair.get<0>());
    }
    auto receive() const -> Completion<ws::WebSocketResult<ws::Message>> {
        auto pair = Completion<ws::WebSocketResult<ws::Message>>::make().unwrap();
        {
            auto f = state->fields.lock().unwrap();
            if (f->terminal.is_some())
                (void)pair.get<1>().complete(Err(*f->terminal));
            else
                f->receiver = Some(rstd::move(pair.get<1>()));
        }
        state->changed.notify();
        return rstd::move(pair.get<0>());
    }
    auto close(Bytes) const -> Completion<ws::WebSocketResult<empty>> {
        auto pair = Completion<ws::WebSocketResult<empty>>::make().unwrap();
        state->fields.lock().unwrap()->closing = true;
        state->stop(ws::WebSocketError::Closed);
        (void)pair.get<1>().complete(Ok(empty {}));
        return rstd::move(pair.get<0>());
    }
    void cancel() const { state->stop(ws::WebSocketError::Cancelled); }
    auto completion() const -> lf::TransferControl { return transfer.control(); }
    auto shutdown() const -> coro<lf::Result<empty>> {
        cancel();
        (void)co_await transfer.control().wait();
        co_return Ok(empty {});
    }
    auto has_receiver() const -> bool { return state->fields.lock().unwrap()->receiver.is_some(); }
    void release_received() const {
        auto f = state->fields.lock().unwrap();
        if (f->receipts > usize()) --f->receipts;
    }
    void discard_received() const {}
    auto has_messages() const -> bool { return state->fields.lock().unwrap()->receipts > usize(); }
    auto failure() const -> Option<WebSocketStatus> {
        auto f = state->fields.lock().unwrap();
        if (f->terminal.is_none()) return None();
        auto status = WebSocketError(*f->terminal).status();
        if (f->diagnostic.is_some()) status.diagnostic = Some(f->diagnostic->clone());
        return Some(rstd::move(status));
    }
    auto take_error() const -> Option<WebSocketError> {
        auto f = state->fields.lock().unwrap();
        if (f->error.is_none()) return None();
        return Some(WebSocketError(*f->terminal).with_cause(f->error.take().unwrap()));
    }
};
struct Backend {
    Arc<State> state;
    bool       finish_during_connect {};
    auto       capabilities() const -> WebSocketCapabilities {
        return { .supported            = true,
                 .receive_backpressure = true,
                 .native_callbacks     = finish_during_connect };
    }
    auto connect(lf::ResourceContext, lf::http::ConnectionPlan, lihttpto::Headers, Vec<String>,
                 WebSocketOptions, WebSocketBackendEvents) const
        -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>>;
};
struct TaskBlocker {
    auto run(lf::Cancellation cancellation) const -> coro<lf::Result<empty>> {
        (void)co_await cancellation.stopped();
        co_return Ok(empty {});
    }
};
} // namespace flow_test
namespace rstd
{
template<>
struct Impl<ncrequest::WebSocketEndpoint, flow_test::Endpoint> : ImplBase<flow_test::Endpoint> {
    auto protocol() const { return this->self().protocol(); }
    auto send(liwebsocketo::Message message) const {
        return this->self().send(rstd::move(message));
    }
    auto receive() const { return this->self().receive(); }
    auto close(ncrequest::Bytes payload) const { return this->self().close(rstd::move(payload)); }
    void cancel() const { this->self().cancel(); }
    auto completion() const { return this->self().completion(); }
    auto shutdown() const { return this->self().shutdown(); }
    auto has_receiver() const { return this->self().has_receiver(); }
    void release_received() const { this->self().release_received(); }
    auto failure() const { return this->self().failure(); }
    auto take_error() const { return this->self().take_error(); }
    void discard_received() const { this->self().discard_received(); }
    auto has_messages() const { return this->self().has_messages(); }
};
template<>
struct Impl<ncrequest::WebSocketBackend, flow_test::Backend> : ImplBase<flow_test::Backend> {
    auto capabilities() const { return this->self().capabilities(); }
    auto connect(lifetchto::ResourceContext resources, lifetchto::http::ConnectionPlan plan,
                 lihttpto::Headers headers, ::alloc::vec::Vec<::alloc::string::String> protocols,
                 ncrequest::WebSocketOptions       options,
                 ncrequest::WebSocketBackendEvents events) const {
        return this->self().connect(rstd::move(resources),
                                    rstd::move(plan),
                                    rstd::move(headers),
                                    rstd::move(protocols),
                                    options,
                                    rstd::move(events));
    }
};
} // namespace rstd
namespace flow_test
{
auto Backend::connect(lf::ResourceContext resources, lf::http::ConnectionPlan, lihttpto::Headers,
                      Vec<String>, WebSocketOptions, WebSocketBackendEvents events) const
    -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>> {
    auto transfer = resources.handle()
                        .submit(lf::PreparedOperation<empty>::make(Driver { state.clone() }))
                        .unwrap();
    (co_await transfer.output()).unwrap();
    if (finish_during_connect) {
        if (events.connected) events.connected();
        state->stop(ws::WebSocketError::Closed);
        if (events.terminal) events.terminal(WebSocketError(WebSocketError::Closed).status());
    }
    co_return Ok(
        Arc<rstd::dyn<WebSocketEndpoint>>::make(Endpoint { state.clone(), rstd::move(transfer) }));
}
auto session(lf::Engine& engine, Arc<State> state, bool finish_during_connect = false)
    -> Arc<Session> {
    auto http = lf::http::NativeBackend::make(engine.resources()).unwrap().backend();
    auto binding =
        ncrequest::BackendBinding::Owned(rstd::move(http),
                                         Some(Arc<rstd::dyn<WebSocketBackend>>::make(Backend {
                                             rstd::move(state), finish_during_connect })));
    return Session::from_backend(engine.handle(), binding).unwrap();
}
auto target() -> DuplexRequest {
    return { lihttpto::Url::parse_http("http://flow.invalid/"_str).unwrap(), {} };
}
auto message(ref<str> data) -> ws::Message {
    return { ws::Kind::Text, Bytes::copy_from_slice(data.as_bytes()) };
}
auto wait_writes(Arc<State> state, usize count) -> coro<empty> {
    for (;;) {
        auto event = state->changed.listen();
        if (state->fields.lock().unwrap()->written.len() >= count) co_return empty {};
        co_await rstd::move(event);
    }
}
} // namespace flow_test
TEST(WebSocketFlow, NativeTerminalBeforeConnectReturnsKeepsThePublicState) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<bool> {
        auto               engine = lf::Engine::make(runtime.handle()).unwrap();
        auto               state  = Arc<State>::make();
        auto               owner  = session(engine, state.clone(), true);
        Vec<int>           sequence;
        WebSocketCallbacks callbacks;
        callbacks.connected = [&sequence] {
            sequence.push(0);
        };
        callbacks.disconnected = [&sequence] {
            sequence.push(1);
        };
        auto client =
            (co_await WebSocketClient::connect(owner.clone(), target(), rstd::move(callbacks)))
                .unwrap();
        bool valid =
            sequence.len() == usize(2) && sequence[usize()] == 0 && sequence[usize(1)] == 1;
        valid         = ! client.is_connected() && valid;
        auto failure  = client.failure();
        valid         = failure.is_some() && *failure == WebSocketError::Closed && valid;
        auto rejected = client.send(message("late"_str));
        valid         = rejected.is_err() && valid;
        if (rejected.is_err())
            valid = rejected.as_ref().unwrap_err().error == WebSocketError::Closed && valid;
        client.disconnect();
        failure = client.failure();
        valid   = failure.is_some() && *failure == WebSocketError::Closed && valid;
        valid   = (co_await client.close()).is_ok() && valid;
        valid   = (co_await client.shutdown()).is_ok() && valid;
        valid   = (co_await owner->close()).is_ok() && valid;
        valid   = engine.resources().usage().bytes == usize() && valid;
        valid   = (co_await engine.close()).is_ok() && valid;
        co_return valid;
    };
    EXPECT_TRUE(runtime.block_on(test()));
}
TEST(WebSocketFlow, QueueBudgetInputReturnCancellationAndFifo) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto             engine = lf::Engine::make(runtime.handle()).unwrap();
        auto             state  = Arc<State>::make();
        auto             owner  = session(engine, state.clone());
        WebSocketOptions options;
        options.send_messages = usize(2);
        options.send_bytes    = usize(6);
        auto wire =
            (co_await WebSocketConnection::connect(owner.clone(), target(), {}, {}, options))
                .unwrap();
        auto first = wire.send(message("one"_str)).unwrap();
        co_await wait_writes(state.clone(), usize(1));
        auto bytes_full = wire.send(message("four"_str)).unwrap_err();
        EXPECT_EQ(bytes_full.error, WebSocketError::QueueFull);
        EXPECT_EQ(bytes_full.error.limit()->maximum, usize(6));
        EXPECT_EQ(bytes_full.error.limit()->observed, usize(7));
        auto second = wire.send(message("two"_str)).unwrap();
        auto full   = wire.send(message("retained"_str)).unwrap_err();
        EXPECT_EQ(full.error, WebSocketError::QueueFull);
        EXPECT_TRUE(full.input.data.as_slice() == "retained"_str.as_bytes());
        second.cancel();
        EXPECT_EQ((co_await second.written()).unwrap_err(), WebSocketError::Cancelled);
        auto third = wire.send(message("end"_str)).unwrap();
        auto timed = co_await rstd::async::timeout(first.written().into_future(), lf::Duration {});
        EXPECT_TRUE(timed.is_err());
        state->finish_write();
        EXPECT_TRUE((co_await first.written()).is_ok());
        first.cancel();
        EXPECT_TRUE(wire.failure().is_none());
        co_await wait_writes(state.clone(), usize(2));
        state->finish_write();
        EXPECT_TRUE((co_await third.written()).is_ok());
        EXPECT_TRUE(state->fields.lock().unwrap()->written[usize(1)].as_slice() ==
                    "end"_str.as_bytes());
        EXPECT_TRUE((co_await wire.close()).is_ok());
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(WebSocketFlow, CloseDeadlineIncludesAnAlreadyRunningWrite) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto             engine = lf::Engine::make(runtime.handle()).unwrap();
        auto             state  = Arc<State>::make();
        auto             owner  = session(engine, state.clone());
        WebSocketOptions options;
        options.close_timeout = lf::Duration::from_millis(u64(15));
        auto wire =
            (co_await WebSocketConnection::connect(owner.clone(), target(), {}, {}, options))
                .unwrap();
        auto first = wire.send(message("active"_str)).unwrap();
        co_await wait_writes(state.clone(), usize(1));
        auto second  = wire.send(message("queued"_str)).unwrap();
        auto closing = wire.close().into_future();
        EXPECT_TRUE((co_await rstd::async::timeout(rstd::future::by_ref(closing),
                                                   lf::Duration::from_millis(u64(1))))
                        .is_err());
        auto closed = co_await wire.close();
        EXPECT_EQ(closed.unwrap_err(), WebSocketError::Timeout);
        EXPECT_EQ((co_await first.written()).unwrap_err(), WebSocketError::Timeout);
        EXPECT_EQ((co_await second.written()).unwrap_err(), WebSocketError::Timeout);
        EXPECT_EQ(wire.send(message("closed"_str)).unwrap_err().error, WebSocketError::Timeout);
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_FALSE(state->fields.lock().unwrap()->closing);
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(WebSocketFlow, ActiveCancellationFailsTheQueueAndReceiveWaitCanResume) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        auto state  = Arc<State>::make();
        auto owner  = session(engine, state.clone());
        auto wire   = (co_await WebSocketConnection::connect(owner.clone(), target())).unwrap();
        {
            auto pending = wire.receive();
            auto timeout =
                co_await rstd::async::timeout(pending.message().into_future(), lf::Duration {});
            EXPECT_TRUE(timeout.is_err());
            EXPECT_TRUE(wire.failure().is_none());
            state->deliver();
            EXPECT_TRUE((co_await pending.message()).is_ok());
            EXPECT_EQ(state->fields.lock().unwrap()->receipts, usize(1));
        }
        EXPECT_EQ(state->fields.lock().unwrap()->receipts, usize());
        auto first = wire.send(message("active"_str)).unwrap();
        co_await wait_writes(state.clone(), usize(1));
        auto second = wire.send(message("queued"_str)).unwrap();
        first.cancel();
        EXPECT_EQ((co_await second.written()).unwrap_err(), WebSocketError::Cancelled);
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_EQ(state->fields.lock().unwrap()->written.len(), usize(1));
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(WebSocketFlow, GracefulCloseSealsAdmissionAndDrainsAcceptedMessages) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        auto state  = Arc<State>::make();
        auto owner  = session(engine, state.clone());
        auto wire   = (co_await WebSocketConnection::connect(owner.clone(), target())).unwrap();
        auto first  = wire.send(message("first"_str)).unwrap();
        auto second = wire.send(message("second"_str)).unwrap();
        EXPECT_TRUE(wire.request_close().is_ok());
        auto rejected = wire.send(message("returned"_str)).unwrap_err();
        EXPECT_EQ(rejected.error, WebSocketError::Closed);
        EXPECT_TRUE(rejected.input.data.as_slice() == "returned"_str.as_bytes());
        co_await wait_writes(state.clone(), usize(1));
        state->finish_write();
        EXPECT_TRUE((co_await first.written()).is_ok());
        co_await wait_writes(state.clone(), usize(2));
        state->finish_write();
        EXPECT_TRUE((co_await second.written()).is_ok());
        EXPECT_TRUE((co_await wire.close()).is_ok());
        EXPECT_TRUE(state->fields.lock().unwrap()->closing);
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}

namespace flow_test
{
auto enqueue(Arc<WebSocketConnection> wire, Arc<rstd::sync::Mutex<Vec<PendingSend>>> accepted)
    -> coro<bool> {
    auto submitted = wire->send(message("x"_str));
    if (submitted.is_err()) {
        auto rejected = rstd::move(submitted).unwrap_err();
        EXPECT_EQ(rejected.error, WebSocketError::QueueFull);
        EXPECT_TRUE(rejected.input.data.as_slice() == "x"_str.as_bytes());
        co_return false;
    }
    accepted->lock().unwrap()->push(rstd::move(submitted).unwrap());
    co_return true;
}
} // namespace flow_test
TEST(WebSocketFlow, ConcurrentCallersShareOneAdmissionBudgetAndWriter) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::multi_thread()
                       .worker_threads(usize(4))
                       .enable_all()
                       .build()
                       .unwrap();
    auto test    = [&]() -> coro<empty> {
        auto             engine = lf::Engine::make(runtime.handle()).unwrap();
        auto             state  = Arc<State>::make();
        auto             owner  = session(engine, state.clone());
        WebSocketOptions options;
        options.send_messages = usize(8);
        options.send_bytes    = usize(8);
        auto wire             = Arc<WebSocketConnection>::make(
            (co_await WebSocketConnection::connect(owner.clone(), target(), {}, {}, options))
                .unwrap());
        auto accepted = Arc<rstd::sync::Mutex<Vec<PendingSend>>>::make(Vec<PendingSend> {});
        Vec<rstd::async::JoinHandle<bool>> callers;
        for (int i {}; i < 32; ++i)
            callers.push(runtime.spawn(enqueue(wire.clone(), accepted.clone())));
        usize count;
        for (auto& caller : callers)
            if ((co_await rstd::future::by_ref(caller)).unwrap()) ++count;
        EXPECT_EQ(count, usize(8));
        for (usize i {}; i < count; ++i) {
            co_await wait_writes(state.clone(), i + usize(1));
            state->finish_write();
        }
        auto pending = [&] {
            auto f = accepted->lock().unwrap();
            return rstd::move(*f);
        }();
        for (auto& item : pending) EXPECT_TRUE((co_await item.written()).is_ok());
        EXPECT_TRUE((co_await wire->close()).is_ok());
        EXPECT_TRUE((co_await wire->shutdown()).is_ok());
        EXPECT_EQ(state->fields.lock().unwrap()->written.len(), usize(8));
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(WebSocketFlow, CancelDiscardsAnUnclaimedCompletedReceive) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto state   = Arc<State>::make();
        auto owner   = session(engine, state.clone());
        auto wire    = (co_await WebSocketConnection::connect(owner.clone(), target())).unwrap();
        auto pending = wire.receive();
        state->deliver();
        pending.cancel();
        EXPECT_EQ((co_await pending.message()).unwrap_err(), WebSocketError::Cancelled);
        EXPECT_EQ(state->fields.lock().unwrap()->receipts, usize());
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(WebSocketFlow, RejectsZeroAndOverflowingLimitsBeforeBackendDispatch) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        auto state  = Arc<State>::make();
        auto owner  = session(engine, state.clone());
        for (int mode {}; mode < 7; ++mode) {
            WebSocketOptions options;
            if (mode == 0)
                options.send_messages = usize();
            else if (mode == 1)
                options.send_bytes = usize();
            else if (mode == 2)
                options.receive_messages = usize();
            else if (mode == 3)
                options.receive_bytes = usize::MAX;
            else if (mode == 4)
                options.limits.frame_bytes = usize();
            else if (mode == 5)
                options.limits.message_bytes = usize();
            else
                options.close_timeout = lf::Duration {};
            auto opened =
                co_await WebSocketConnection::connect(owner.clone(), target(), {}, {}, options);
            EXPECT_TRUE(opened.is_err());
            if (opened.is_err()) {
                auto error = rstd::move(opened).unwrap_err();
                EXPECT_TRUE(error.admission.is_some() && error.admission->is_InvalidOptions());
            }
        }
        EXPECT_TRUE(state->fields.lock().unwrap()->written.is_empty());
        EXPECT_EQ(engine.resources().usage().operations, usize());
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(WebSocketFlow, WriterAndCallbackAdmissionFailuresPreserveCleanupReports) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        for (int mode {}; mode < 2; ++mode) {
            lf::EngineOptions options;
            if (mode)
                options.limits.tasks = usize(1);
            else
                options.limits.operations = usize(1);
            auto                    engine = lf::Engine::make(runtime.handle(), options).unwrap();
            auto                    state  = Arc<State>::make();
            auto                    owner  = session(engine, state.clone());
            Option<lf::TaskControl> blocker;
            if (mode) blocker = Some(engine.resources().spawn(TaskBlocker {}).unwrap());
            Option<WebSocketConnectFailure> failure;
            if (mode) {
                auto opened = co_await WebSocketClient::connect(owner.clone(), target(), {});
                EXPECT_TRUE(opened.is_err());
                if (opened.is_err()) failure = Some(rstd::move(opened).unwrap_err());
            } else {
                auto opened = co_await WebSocketConnection::connect(owner.clone(), target());
                EXPECT_TRUE(opened.is_err());
                if (opened.is_err()) failure = Some(rstd::move(opened).unwrap_err());
            }
            if (failure.is_some()) {
                EXPECT_TRUE(failure->admission.is_some() && failure->admission->is_Capacity());
                EXPECT_TRUE(failure->report.is_some() && (*failure->report)->cleanup.is_ok());
            }
            if (blocker.is_some()) {
                blocker->cancel();
                (void)co_await blocker->wait();
            }
            EXPECT_EQ(engine.resources().usage().tasks, usize());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await owner->close()).is_ok());
            EXPECT_TRUE((co_await engine.close()).is_ok());
        }
        co_return empty {};
    };
    runtime.block_on(test());
}

TEST(WebSocketFlow, TerminalCauseHasOneOwnerAndDiagnosticsSurviveTakingIt) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto state   = Arc<State>::make();
        auto owner   = session(engine, state.clone());
        auto wire    = (co_await WebSocketConnection::connect(owner.clone(), target())).unwrap();
        auto pending = wire.receive();
        {
            auto f        = state->fields.lock().unwrap();
            f->error      = Some(lf::Error::Connect(rstd::io::Error::from_raw_os_error(i32(32))));
            f->diagnostic = Some(lf::report_failure(*f->error));
        }
        state->stop(ws::WebSocketError::Transport);
        auto received = co_await pending.message();
        EXPECT_TRUE(received.is_err());
        EXPECT_TRUE(received.as_ref().unwrap_err().cause().is_none());
        auto report = co_await wire.completion().wait();
        auto status = wire.failure().unwrap();
        EXPECT_TRUE(status.diagnostic.is_some());
        EXPECT_EQ(report->logical.result.as_ref().unwrap_err().message, status.diagnostic->message);
        {
            auto error = wire.take_error();
            EXPECT_TRUE(error.is_some() && error->cause().is_some());
            if (error.is_some() && error->cause().is_some()) {
                EXPECT_TRUE(error->cause()->is_Connect());
                if (error->cause()->is_Connect())
                    EXPECT_EQ(error->cause()->as_Connect().cause.raw_os_error(), Some(i32(32)));
            }
        }
        EXPECT_TRUE(wire.take_error().is_none());
        EXPECT_EQ(wire.failure()->diagnostic->message, status.diagnostic->message);
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}

TEST(WebSocketFlow, CallbacksConsumeMessagesAndReportTerminalInOrder) {
    using namespace flow_test;
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        for (bool failure : array<bool, 2> { false, true }) {
            auto               engine = lf::Engine::make(runtime.handle()).unwrap();
            auto               state  = Arc<State>::make();
            auto               owner  = session(engine, state.clone());
            Vec<int>           sequence;
            WebSocketCallbacks callbacks;
            callbacks.connected = [&sequence] {
                sequence.push(0);
            };
            callbacks.message = [&sequence](ws::Message value) {
                EXPECT_TRUE(value.data.as_slice() == "payload"_str.as_bytes());
                sequence.push(1);
            };
            callbacks.error = [&sequence](WebSocketStatus error) {
                EXPECT_EQ(error, WebSocketError::Overflow);
                sequence.push(2);
            };
            callbacks.disconnected = [&sequence] {
                sequence.push(3);
            };
            auto client =
                (co_await WebSocketClient::connect(owner.clone(), target(), rstd::move(callbacks)))
                    .unwrap();
            for (;;) {
                auto changed = state->changed.listen();
                if (state->fields.lock().unwrap()->receiver.is_some()) break;
                co_await rstd::move(changed);
            }
            state->deliver();
            while (sequence.len() < usize(2)) co_await rstd::async::yield_now();
            state->stop(failure ? ws::WebSocketError::Overflow : ws::WebSocketError::Closed);
            while (sequence.len() < usize(failure ? 4 : 3)) co_await rstd::async::yield_now();
            EXPECT_EQ(sequence[usize()], 0);
            EXPECT_EQ(sequence[usize(1)], 1);
            if (failure) EXPECT_EQ(sequence[usize(2)], 2);
            EXPECT_EQ(sequence[sequence.len() - usize(1)], 3);
            EXPECT_FALSE(client.is_connected());
            EXPECT_TRUE((co_await client.wait_shutdown()).is_ok());
            auto shutdown = client.take_shutdown_result();
            EXPECT_TRUE(shutdown.is_some() && shutdown->is_ok());
            EXPECT_TRUE((co_await client.wait_shutdown()).is_ok());
            EXPECT_TRUE(client.take_shutdown_result().is_none());
            EXPECT_EQ(state->fields.lock().unwrap()->receipts, usize());
            EXPECT_TRUE((co_await owner->close()).is_ok());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
        }
        co_return empty {};
    };
    runtime.block_on(test());
}
