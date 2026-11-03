module ncrequest;
import :websocket;
using namespace rstd::prelude;
namespace ncrequest
{
namespace lf = lifetchto;
struct CallbackFields {
    usize active {};
    bool  connected {}, ended {}, stopped {};
};
struct CallbackState {
    WebSocketCallbacks                callbacks;
    rstd::sync::Mutex<CallbackFields> fields { CallbackFields {} };
    lf::Event                         changed;
    explicit CallbackState(WebSocketCallbacks callbacks): callbacks(rstd::move(callbacks)) {}
    void invoke(client::Callback<void()> work) {
        if (! work) return;
        {
            auto f = fields.lock().unwrap();
            if (f->stopped) return;
            ++f->active;
        }
        work();
        {
            auto f = fields.lock().unwrap();
            --f->active;
        }
        changed.notify();
    }
    void connected() {
        {
            auto f = fields.lock().unwrap();
            if (f->ended || f->stopped) return;
            f->connected = true;
        }
        invoke(callbacks.connected.clone());
    }
    void message(liwebsocketo::Message value) {
        if (callbacks.message)
            invoke([callback = callbacks.message.clone(), value = rstd::move(value)]() mutable {
                callback(rstd::move(value));
            });
    }
    void error(WebSocketStatus value) {
        if (callbacks.error)
            invoke([callback = callbacks.error.clone(), value = rstd::move(value)]() mutable {
                callback(rstd::move(value));
            });
    }
    void disconnected() {
        {
            auto f = fields.lock().unwrap();
            if (f->ended) return;
            f->ended     = true;
            f->connected = false;
        }
        invoke(callbacks.disconnected.clone());
    }
    void stop() {
        auto f       = fields.lock().unwrap();
        f->stopped   = true;
        f->connected = false;
    }
};
auto drain_callbacks(Arc<CallbackState> state) -> coro<void> {
    for (;;) {
        auto changed = state->changed.listen();
        if (state->fields.lock().unwrap()->active == usize()) co_return;
        co_await rstd::move(changed);
    }
}
struct ClientFields {
    Option<lf::TaskControl> worker;
    bool                    launched {}, stopped {};
};
struct WebSocketClientState {
    WebSocketConnection             wire;
    Arc<CallbackState>              callbacks;
    rstd::sync::Mutex<ClientFields> fields { ClientFields {} };
    lf::Event                       changed;
    WebSocketClientState(WebSocketConnection wire, Arc<CallbackState> callbacks)
        : wire(rstd::move(wire)), callbacks(rstd::move(callbacks)) {}
    void stop() const {
        fields.lock().unwrap()->stopped = true;
        callbacks->stop();
        wire.cancel();
        changed.notify();
    }
};
struct CallbackReader {
    Arc<WebSocketClientState> state;
    void                      request_stop(lf::StopReason) const { state->stop(); }
    auto                      run(lf::Cancellation) const -> coro<lf::Result<empty>> {
        state->callbacks->connected();
        for (;;) {
            if (state->fields.lock().unwrap()->stopped) break;
            auto pending  = state->wire.receive();
            auto received = co_await pending.message();
            if (received.is_err()) {
                auto error = rstd::move(received).unwrap_err();
                if (error != WebSocketError::Closed && error != WebSocketError::Cancelled)
                    state->callbacks->error(error.status());
                break;
            }
            state->callbacks->message(rstd::move(received).unwrap());
        }
        state->callbacks->disconnected();
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
        co_await drain_callbacks(state->callbacks.clone());
        (void)co_await state->wire.shutdown();
        co_return lf::collect_errors(state->wire.take_cleanup_errors());
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
    auto               receiver = Arc<CallbackState>::make(rstd::move(callbacks));
    bool               native   = session && session->websocket_capabilities().native_callbacks;
    WebSocketCallbacks hooks;
    if (native) {
        auto weak       = receiver.downgrade();
        hooks.connected = [weak = weak.clone()] {
            if (auto state = weak.upgrade()) state->connected();
        };
        hooks.message = [weak = weak.clone()](liwebsocketo::Message value) {
            if (auto state = weak.upgrade()) state->message(rstd::move(value));
        };
        hooks.error = [weak = weak.clone()](WebSocketStatus value) {
            if (auto state = weak.upgrade()) state->error(rstd::move(value));
        };
        hooks.disconnected = [weak = rstd::move(weak)] {
            if (auto state = weak.upgrade()) state->disconnected();
        };
    }
    auto opened = co_await WebSocketConnection::connect_with_events(session.clone(),
                                                                    rstd::move(target),
                                                                    rstd::move(headers),
                                                                    rstd::move(protocols),
                                                                    options,
                                                                    rstd::move(hooks));
    if (opened.is_err()) {
        receiver->stop();
        co_await drain_callbacks(receiver.clone());
        co_return Err(rstd::move(opened).unwrap_err());
    }
    if (! native) receiver->fields.lock().unwrap()->connected = true;
    auto state = Arc<WebSocketClientState>::make(rstd::move(opened).unwrap(), rstd::move(receiver));
    auto resources = session->resources();
    auto owner     = Arc<rstd::dyn<lf::ManagedResource>>::make(CallbackOwner { state.clone() });
    auto attached  = resources.attach(empty {}, owner.clone());
    if (attached.is_err()) {
        state->fields.lock().unwrap()->launched = true;
        auto released                           = co_await owner->release({});
        if (released.is_err()) resources.record_cleanup_error(rstd::move(released).unwrap_err());
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
        else if (! native) {
            auto worker = resources.spawn(CallbackReader { state.clone() });
            if (worker.is_err())
                error = Some(rstd::move(worker).unwrap_err().error);
            else
                f->worker = Some(rstd::move(worker).unwrap());
        }
    }
    if (error.is_some()) {
        auto released = co_await registration->release();
        if (released.is_err()) resources.record_cleanup_error(rstd::move(released).unwrap_err());
        co_return Err(
            WebSocketConnectFailure { .admission = Some(error.take().unwrap()),
                                      .report = Some(co_await state->wire.completion().wait()) });
    }
    co_return Ok(WebSocketClient(rstd::move(state), rstd::move(registration)));
}
auto WebSocketClient::is_connected() const -> bool {
    auto f = state_->callbacks->fields.lock().unwrap();
    return f->connected && ! f->stopped && ! f->ended && state_->wire.failure().is_none();
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
auto WebSocketClient::failure() const -> Option<WebSocketStatus> { return state_->wire.failure(); }
auto WebSocketClient::take_error() const -> Option<WebSocketError> {
    return state_->wire.take_error();
}
void WebSocketClient::disconnect() const { state_->stop(); }
auto WebSocketClient::shutdown() const -> coro<lf::Result<empty>> {
    return registration_->release();
}
auto WebSocketClient::wait_shutdown() const -> coro<lf::CleanupReport> {
    return registration_->wait_released();
}
auto WebSocketClient::take_shutdown_result() const -> Option<lf::Result<empty>> {
    return registration_->take_release_result();
}
} // namespace ncrequest
