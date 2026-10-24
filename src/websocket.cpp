module ncrequest;
import :websocket;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest
{
namespace lf = lifetchto;
struct DeliveryFields {
    Option<client::Callback<void()>> work;
    bool                             started {}, done {}, rejected {};
};
struct CallbackDelivery {
    rstd::sync::Mutex<DeliveryFields> fields;
    lf::Event                         changed;
    explicit CallbackDelivery(client::Callback<void()> work)
        : fields(DeliveryFields { Some(rstd::move(work)) }) {}
    void cancel(bool rejected = false) {
        {
            auto f = fields.lock().unwrap();
            if (f->started || f->done) return;
            f->work     = None();
            f->done     = true;
            f->rejected = rejected;
        }
        changed.notify();
    }
    void run() {
        Option<client::Callback<void()>> work;
        {
            auto f = fields.lock().unwrap();
            if (f->done) return;
            f->started = true;
            work       = f->work.take();
        }
        (*work)();
        {
            auto f  = fields.lock().unwrap();
            f->done = true;
        }
        changed.notify();
    }
};
struct CallbackFields {
    Option<lf::TaskControl>       worker;
    Option<Arc<CallbackDelivery>> delivery;
    Option<WebSocketError>        failure;
    bool                          connected { true }, launched {}, stopped {};
};
struct WebSocketClientState {
    WebSocketConnection               wire;
    WebSocketCallbacks                callbacks;
    rstd::sync::Mutex<CallbackFields> fields { CallbackFields {} };
    lf::Event                         changed;
    WebSocketClientState(WebSocketConnection wire, WebSocketCallbacks callbacks)
        : wire(rstd::move(wire)), callbacks(rstd::move(callbacks)) {}
    void fail(WebSocketError error) {
        auto previous = wire.failure();
        if (previous.is_some() && *previous != WebSocketError::Closed &&
            *previous != WebSocketError::Cancelled &&
            *previous != WebSocketError::CallbackExecutorClosed)
            error = rstd::move(error).with_cause(lf::Error::Extension(
                "websocket"_Str, Arc<rstd::dyn<rstd::error::Error>>::make(previous->clone())));
        {
            auto f = fields.lock().unwrap();
            if (f->failure.is_none()) f->failure = Some(error.clone());
            f->stopped = true;
        }
        wire.fail(rstd::move(error));
        changed.notify();
    }
    void stop() const {
        Option<Arc<CallbackDelivery>> delivery;
        {
            auto f     = fields.lock().unwrap();
            f->stopped = true;
            if (f->delivery.is_some()) delivery = Some(f->delivery->clone());
        }
        if (delivery.is_some()) (*delivery)->cancel();
        wire.cancel();
        changed.notify();
    }
};
auto deliver_callback(Arc<WebSocketClientState> state, client::Callback<void()> work)
    -> coro<WebSocketResult<empty>> {
    if (! work) co_return Ok(empty {});
    auto delivery = Arc<CallbackDelivery>::make(rstd::move(work));
    {
        auto f = state->fields.lock().unwrap();
        if (f->stopped) co_return Err(WebSocketError(WebSocketError::Cancelled));
        f->delivery = Some(delivery.clone());
    }
    if (state->callbacks.executor.is_some()) {
        auto job = rstd::async::ExecutorJob::make(
            [delivery = delivery.clone()] {
                delivery->run();
            },
            [delivery = delivery.clone()] {
                delivery->cancel(true);
            });
        if (! state->callbacks.executor->post_job(rstd::move(job))) delivery->cancel(true);
    } else
        delivery->run();
    bool rejected;
    for (;;) {
        auto event = delivery->changed.listen();
        {
            auto f = delivery->fields.lock().unwrap();
            if (f->done) {
                rejected = f->rejected;
                break;
            }
        }
        co_await rstd::move(event);
    }
    state->fields.lock().unwrap()->delivery = None();
    if (rejected) {
        state->fail(WebSocketError::CallbackExecutorClosed);
        co_return Err(WebSocketError(WebSocketError::CallbackExecutorClosed));
    }
    co_return Ok(empty {});
}
struct CallbackReader {
    Arc<WebSocketClientState> state;
    void                      request_stop(lf::StopReason) const { state->stop(); }
    auto                      run(lf::Cancellation) const -> coro<lf::Result<empty>> {
        auto connected =
            co_await deliver_callback(state.clone(), state->callbacks.connected.clone());
        if (connected.is_ok())
            for (;;) {
                auto pending  = state->wire.receive();
                auto received = co_await pending.message();
                if (received.is_err()) {
                    auto error = rstd::move(received).unwrap_err();
                    if (error != WebSocketError::Closed && error != WebSocketError::Cancelled &&
                        state->callbacks.error) {
                        auto work =
                            client::Callback<void()>([callback = state->callbacks.error.clone(),
                                                      error    = rstd::move(error)]() mutable {
                                callback(rstd::move(error));
                            });
                        (void)co_await deliver_callback(state.clone(), rstd::move(work));
                    }
                    break;
                }
                if (state->callbacks.message) {
                    auto work = client::Callback<void()>(
                        [callback = state->callbacks.message.clone(),
                         message  = rstd::move(received).unwrap()]() mutable {
                            callback(rstd::move(message));
                        });
                    if ((co_await deliver_callback(state.clone(), rstd::move(work))).is_err())
                        break;
                }
            }
        state->fields.lock().unwrap()->connected = false;
        (void)co_await deliver_callback(state.clone(), state->callbacks.disconnected.clone());
        co_return Ok(empty {});
    }
};
struct CallbackOwner {
    Arc<WebSocketClientState> state;
    void                      request_stop(lf::StopReason) const { state->stop(); }
    auto                      release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        state->stop();
        for (;;) {
            auto event = state->changed.listen();
            if (state->fields.lock().unwrap()->launched) break;
            co_await rstd::move(event);
        }
        auto worker = [&] {
            auto f = state->fields.lock().unwrap();
            return f->worker.is_some() ? Some(f->worker->clone()) : None<lf::TaskControl>();
        }();
        if (worker.is_some()) (void)co_await worker->wait();
        co_return co_await state->wire.shutdown();
    }
};
struct CallbackLaunch {
    Arc<WebSocketClientState> state;
    ~CallbackLaunch() {
        state->fields.lock().unwrap()->launched = true;
        state->changed.notify();
    }
};
} // namespace ncrequest
namespace rstd
{
template<>
struct Impl<lifetchto::ManagedResource, ncrequest::CallbackOwner>
    : ImplBase<ncrequest::CallbackOwner> {
    void request_stop(lifetchto::StopReason reason) const { this->self().request_stop(reason); }
    auto release(lifetchto::ReleaseContext context) const { return this->self().release(context); }
};
} // namespace rstd
namespace ncrequest
{
WebSocketClient::WebSocketClient(Arc<WebSocketClientState>     state,
                                 Arc<lf::ResourceLease<empty>> registration)
    : state_(rstd::move(state)), registration_(rstd::move(registration)) {}
WebSocketClient::WebSocketClient(WebSocketClient&&) noexcept = default;
auto WebSocketClient::operator=(WebSocketClient&& other) noexcept -> WebSocketClient& {
    if (this != &other) {
        if (state_) disconnect();
        state_        = rstd::move(other.state_);
        registration_ = rstd::move(other.registration_);
    }
    return *this;
}
WebSocketClient::~WebSocketClient() {
    if (state_) disconnect();
}
auto WebSocketClient::connect(Arc<Session> session, DuplexRequest target,
                              WebSocketCallbacks callbacks, lihttpto::Headers headers,
                              Vec<String> protocols, WebSocketOptions options)
    -> coro<rstd::Result<WebSocketClient, WebSocketConnectFailure>> {
    auto opened = co_await WebSocketConnection::connect(
        session.clone(), rstd::move(target), rstd::move(headers), rstd::move(protocols), options);
    if (opened.is_err()) co_return Err(rstd::move(opened).unwrap_err());
    auto state =
        Arc<WebSocketClientState>::make(rstd::move(opened).unwrap(), rstd::move(callbacks));
    auto resources = session->resources();
    auto owner     = Arc<rstd::dyn<lf::ManagedResource>>::make(CallbackOwner { state.clone() });
    auto attached  = resources.attach(empty {}, owner.clone());
    if (attached.is_err()) {
        state->fields.lock().unwrap()->launched = true;
        (void)co_await owner->release({});
        co_return Err(
            WebSocketConnectFailure { .admission = Some(rstd::move(attached).unwrap_err().error),
                                      .report = Some(co_await state->wire.completion().wait()) });
    }
    auto registration = Arc<lf::ResourceLease<empty>>::make(rstd::move(attached).unwrap());
    Option<lf::Error> error;
    {
        CallbackLaunch launch { state.clone() };
        auto           f = state->fields.lock().unwrap();
        if (f->stopped)
            error = Some(lf::Error::Closed());
        else {
            auto worker = resources.spawn(CallbackReader { state.clone() });
            if (worker.is_err())
                error = Some(rstd::move(worker).unwrap_err().error);
            else
                f->worker = Some(rstd::move(worker).unwrap());
        }
    }
    if (error.is_some()) {
        (void)co_await registration->release();
        co_return Err(
            WebSocketConnectFailure { .admission = Some(error.take().unwrap()),
                                      .report = Some(co_await state->wire.completion().wait()) });
    }
    co_return Ok(WebSocketClient(rstd::move(state), rstd::move(registration)));
}
auto WebSocketClient::is_connected() const -> bool {
    auto f = state_->fields.lock().unwrap();
    return f->connected && ! f->stopped;
}
auto WebSocketClient::send(liwebsocketo::Message message) const
    -> rstd::Result<PendingSend, RejectedMessage> {
    return state_->wire.send(rstd::move(message));
}
auto WebSocketClient::request_close(Bytes payload) const -> WebSocketResult<empty> {
    return state_->wire.request_close(rstd::move(payload));
}
auto WebSocketClient::close(Bytes payload) const -> coro<WebSocketResult<empty>> {
    return state_->wire.close(rstd::move(payload));
}
auto WebSocketClient::failure() const -> Option<WebSocketError> {
    {
        auto f = state_->fields.lock().unwrap();
        if (f->failure.is_some()) return Some(f->failure->clone());
    }
    return state_->wire.failure();
}
void WebSocketClient::disconnect() const { state_->stop(); }
auto WebSocketClient::shutdown() const -> coro<lf::Result<empty>> {
    return registration_->release();
}
} // namespace ncrequest
