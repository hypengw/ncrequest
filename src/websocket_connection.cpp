module ncrequest;
import :websocket_connection;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest
{
namespace lf = lifetchto;
struct NativeWebSocketWireState {
    UpgradeConnection                    wire;
    rstd::sync::Mutex<Option<lf::Error>> failure { None<lf::Error>() };
    explicit NativeWebSocketWireState(UpgradeConnection wire): wire(rstd::move(wire)) {}
    void remember(const lf::Error& error) {
        auto f = failure.lock().unwrap();
        if (f->is_none()) *f = Some(error.clone());
    }
};
struct NativeWebSocketWire {
    Arc<NativeWebSocketWireState> state;
    static auto read_from(Arc<NativeWebSocketWireState> state) -> coro<lf::Result<Bytes>> {
        auto result = co_await state->wire.read();
        if (result.is_err()) state->remember(result.as_ref().unwrap_err());
        co_return rstd::move(result);
    }
    static auto write_to(Arc<NativeWebSocketWireState> state, Bytes bytes)
        -> coro<lf::Result<usize>> {
        auto result = co_await state->wire.write(rstd::move(bytes));
        if (result.is_err()) state->remember(result.as_ref().unwrap_err());
        co_return rstd::move(result);
    }
    auto read() const { return read_from(state.clone()); }
    auto write(Bytes bytes) const { return write_to(state.clone(), rstd::move(bytes)); }
    auto finish() const { return state->wire.finish(); }
    void cancel() const { state->wire.cancel(); }
};
using WebSocketDriver = liwebsocketo::WebSocketConnection<NativeWebSocketWire>;
struct NativeWebSocketState {
    WebSocketDriver               driver;
    Arc<NativeWebSocketWireState> wire;
    lf::TransferControl           control;
    Option<lf::ResourcePermit>    tasks;
    rstd::sync::Mutex<empty>      access { empty {} };
    NativeWebSocketState(WebSocketDriver driver, Arc<NativeWebSocketWireState> wire,
                         lf::TransferControl control)
        : driver(rstd::move(driver)), wire(rstd::move(wire)), control(rstd::move(control)) {}
};
struct WebSocketOwner {
    Arc<NativeWebSocketState> state;
    void                      request_stop(lf::StopReason) const {
        auto lock = state->access.lock().unwrap();
        state->driver.cancel();
    }
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        auto waiting = [&] {
            auto lock = state->access.lock().unwrap();
            return state->driver.shutdown();
        }();
        co_await rstd::move(waiting);
        state->tasks  = None();
        auto terminal = state->driver.terminal();
        auto report   = co_await (terminal == Some(liwebsocketo::WebSocketError::Closed)
                                      ? state->control.wait()
                                      : state->control.close());
        if (report->cleanup.is_err()) co_return Err(report->cleanup.errors[usize()].clone());
        co_return Ok(empty {});
    }
};
struct NativeEndpoint {
    Arc<NativeWebSocketState>     state;
    Arc<lf::ResourceLease<empty>> registration;
    auto                          protocol() const -> Option<String> {
        auto guard = state->access.lock().unwrap();
        return state->driver.protocol().clone();
    }
    auto send(liwebsocketo::Message message) const
        -> rstd::async::Completion<liwebsocketo::WebSocketResult<empty>> {
        auto guard = state->access.lock().unwrap();
        return state->driver.send(rstd::move(message));
    }
    auto receive() const
        -> rstd::async::Completion<liwebsocketo::WebSocketResult<liwebsocketo::Message>> {
        auto guard = state->access.lock().unwrap();
        return state->driver.receive();
    }
    auto close(Bytes payload) const
        -> rstd::async::Completion<liwebsocketo::WebSocketResult<empty>> {
        auto guard = state->access.lock().unwrap();
        return state->driver.close(rstd::move(payload));
    }
    void cancel() const {
        auto guard = state->access.lock().unwrap();
        state->driver.cancel();
    }
    auto completion() const -> lf::TransferControl { return state->control.clone(); }
    auto shutdown() const -> coro<lf::Result<empty>> { return registration->release(); }
    void release_received() const { state->driver.release_received(); }
    void discard_received() const { state->driver.discard_received(); }
    auto has_messages() const -> bool { return state->driver.has_messages(); }
    auto failure() const -> Option<WebSocketError> {
        auto violation = state->driver.limit_violation();
        if (violation.is_some()) {
            auto kind = violation->kind;
            auto code = kind == liwebsocketo::LimitKind::Frame     ? WebSocketError::FrameTooLarge
                        : kind == liwebsocketo::LimitKind::Message ? WebSocketError::MessageTooLarge
                        : kind == liwebsocketo::LimitKind::ReceiveMessages
                            ? WebSocketError::ReceiveMessagesFull
                            : WebSocketError::ReceiveBytesFull;
            return Some(WebSocketError(code,
                                       usize(violation->maximum.to_primitive()),
                                       usize(violation->observed.to_primitive())));
        }
        auto terminal = state->driver.terminal();
        if (terminal.is_none()) return None();
        auto error = WebSocketError(*terminal).with_protocol(state->driver.protocol_error());
        if (error == WebSocketError::Transport) {
            auto cause = state->wire->failure.lock().unwrap();
            if (cause->is_some()) {
                if ((*cause)->is_Timeout())
                    error = WebSocketError(WebSocketError::Timeout);
                else if ((*cause)->is_Canceled())
                    error = WebSocketError(WebSocketError::Cancelled);
                return Some(rstd::move(error).with_cause((*cause)->clone()));
            }
        }
        return Some(rstd::move(error));
    }
    auto has_receiver() const -> bool {
        auto guard = state->access.lock().unwrap();
        return state->driver.has_receiver();
    }
};
} // namespace ncrequest
namespace rstd
{
template<>
struct Impl<lifetchto::ManagedResource, ncrequest::WebSocketOwner>
    : ImplBase<ncrequest::WebSocketOwner> {
    void request_stop(lifetchto::StopReason reason) const { this->self().request_stop(reason); }
    auto release(lifetchto::ReleaseContext context) const { return this->self().release(context); }
};
template<>
struct Impl<ncrequest::WebSocketEndpoint, ncrequest::NativeEndpoint>
    : ImplBase<ncrequest::NativeEndpoint> {
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
    void discard_received() const { this->self().discard_received(); }
    auto has_messages() const { return this->self().has_messages(); }
};
} // namespace rstd
namespace ncrequest
{
auto connect_native_websocket(Arc<Session> session, DuplexRequest target, lihttpto::Headers headers,
                              Vec<String> protocols, WebSocketOptions options)
    -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>> {
    if (! session || target.url.http_origin().is_none() || options.receive_messages == usize() ||
        options.receive_bytes == usize())
        co_return Err(WebSocketConnectFailure {
            .connection = Some(WebSocketError(WebSocketError::Protocol)) });
    array<u8, 16> nonce;
    if (rstd::random::fill_secure(nonce.as_mut_slice()).is_err())
        co_return Err(
            WebSocketConnectFailure { .connection = Some(WebSocketError(WebSocketError::Random)) });
    auto handshake =
        liwebsocketo::ClientHandshake::make(target.url.request_target().as_str(),
                                            target.url.http_origin()->authority().as_str(),
                                            nonce,
                                            rstd::move(headers),
                                            rstd::move(protocols));
    if (handshake.is_err())
        co_return Err(
            WebSocketConnectFailure { .handshake = Some(rstd::move(handshake).unwrap_err()) });
    auto pending =
        session->upgrade({ rstd::move(target), handshake->request().clone() }, options.upgrade);
    if (pending.is_err())
        co_return Err(
            WebSocketConnectFailure { .admission = Some(rstd::move(pending).unwrap_err().error) });
    auto opened = co_await pending->opened();
    if (opened.is_err())
        co_return Err(WebSocketConnectFailure { .upgrade = Some(rstd::move(opened).unwrap_err()) });
    auto wire      = rstd::move(opened).unwrap();
    auto control   = wire.completion();
    auto validated = handshake->validate(wire.response());
    if (validated.is_err()) {
        auto report = co_await wire.close();
        co_return Err(
            WebSocketConnectFailure { .handshake = Some(rstd::move(validated).unwrap_err()),
                                      .report    = Some(rstd::move(report)) });
    }
    auto protocol_options            = static_cast<liwebsocketo::WebSocketOptions>(options);
    protocol_options.retain_received = true;
    auto native_wire                 = Arc<NativeWebSocketWireState>::make(rstd::move(wire));
    auto prepared = WebSocketDriver::prepare(NativeWebSocketWire { native_wire.clone() },
                                             liwebsocketo::Role::Client,
                                             protocol_options,
                                             rstd::move(validated).unwrap());
    if (prepared.is_err()) {
        auto report = co_await control.close();
        co_return Err(
            WebSocketConnectFailure { .connection = Some(WebSocketError(prepared.unwrap_err())),
                                      .report     = Some(rstd::move(report)) });
    }
    auto state = Arc<NativeWebSocketState>::make(
        rstd::move(prepared).unwrap(), rstd::move(native_wire), control.clone());
    auto resources = session->resources();
    auto permit    = resources.try_reserve(lf::ResourceAmount {
        .tasks = usize(3), .bytes = options.limits.message_bytes + options.limits.frame_bytes });
    if (permit.is_err()) {
        (void)co_await state->driver.shutdown();
        co_return Err(WebSocketConnectFailure { .admission = Some(rstd::move(permit).unwrap_err()),
                                                .report    = Some(co_await control.close()) });
    }
    state->tasks  = Some(rstd::move(permit).unwrap());
    auto owner    = Arc<rstd::dyn<lf::ManagedResource>>::make(WebSocketOwner { state.clone() });
    auto attached = resources.attach(empty {}, owner.clone());
    if (attached.is_err()) {
        auto error = rstd::move(attached).unwrap_err().error;
        (void)co_await owner->release({});
        co_return Err(WebSocketConnectFailure { .admission = Some(rstd::move(error)),
                                                .report    = Some(co_await control.wait()) });
    }
    auto registration = Arc<lf::ResourceLease<empty>>::make(rstd::move(attached).unwrap());
    auto launched     = [&] {
        auto lock = state->access.lock().unwrap();
        return state->driver.launch(resources.runtime());
    }();
    if (launched.is_err()) {
        (void)co_await registration->release();
        co_return Err(
            WebSocketConnectFailure { .connection = Some(WebSocketError(launched.unwrap_err())),
                                      .report     = Some(co_await control.wait()) });
    }
    co_return Ok(Arc<rstd::dyn<WebSocketEndpoint>>::make(
        NativeEndpoint { rstd::move(state), rstd::move(registration) }));
}
struct WebSocketSendFields {
    Option<liwebsocketo::Message>  message;
    Option<WebSocketResult<empty>> result;
    bool                           started {};
    Option<lf::ResourcePermit>     permit;
};
struct WebSocketSendState {
    rstd::sync::Mutex<WebSocketSendFields> fields;
    usize                                  bytes;
    lf::Event                              changed;
    WebSocketSendState(liwebsocketo::Message message, lf::ResourcePermit permit)
        : fields(WebSocketSendFields {
              Some(rstd::move(message)), {}, false, Some(rstd::move(permit)) }),
          bytes(fields.lock().unwrap()->message->data.len()) {}
};
struct WebSocketReceiveFields {
    Option<rstd::async::Completion<liwebsocketo::WebSocketResult<liwebsocketo::Message>>> pending;
    Option<WebSocketError>                                                                rejected;
    bool reading {}, done {}, delivered {};
};
struct WebSocketReceiveState {
    rstd::sync::Mutex<WebSocketReceiveFields> fields { WebSocketReceiveFields {} };
};
struct WebSocketFields {
    Vec<Arc<WebSocketSendState>>       sends;
    usize                              bytes {};
    Option<Arc<WebSocketReceiveState>> receiver;
    bool                               stream {}, closing {};
    Bytes                              close_payload;
    Option<lf::Instant>                close_deadline;
    Option<WebSocketResult<empty>>     close_result;
    Option<WebSocketError>             terminal;
    Option<lf::ResourcePermit>         receive_budget;
};
auto websocket_transport_error(const WebSocketError& error) -> lf::Error {
    if (error == WebSocketError::Cancelled) return lf::Error::Canceled();
    if (error == WebSocketError::Timeout) return lf::Error::Timeout();
    return lf::Error::Extension("websocket"_Str,
                                Arc<rstd::dyn<rstd::error::Error>>::make(error.clone()));
}
struct WebSocketState {
    Arc<rstd::dyn<WebSocketEndpoint>>  endpoint;
    lf::ResourceContext                resources;
    WebSocketOptions                   options;
    rstd::sync::Mutex<WebSocketFields> fields { WebSocketFields {} };
    lf::Event                          changed;
    WebSocketState(Arc<rstd::dyn<WebSocketEndpoint>> endpoint, lf::ResourceContext resources,
                   WebSocketOptions options, lf::ResourcePermit receive_budget)
        : endpoint(rstd::move(endpoint)), resources(rstd::move(resources)), options(options) {
        fields.lock().unwrap()->receive_budget = Some(rstd::move(receive_budget));
    }
    void release_receive_budget() {
        if (endpoint->completion().snapshot().phase != lf::TransferPhase::Released ||
            endpoint->has_messages())
            return;
        auto f = fields.lock().unwrap();
        if (f->terminal.is_some()) f->receive_budget = None();
    }
    void discard_received() {
        Option<Arc<WebSocketReceiveState>> receiver;
        Option<WebSocketError>             error;
        {
            auto f = fields.lock().unwrap();
            if (f->receiver.is_some()) receiver = Some(f->receiver->clone());
            error = f->terminal.clone();
        }
        bool release = false;
        if (receiver.is_some()) {
            auto f = (*receiver)->fields.lock().unwrap();
            if (! f->done) {
                release     = f->pending.is_some();
                f->pending  = None();
                f->rejected = Some(error.is_some() ? error->clone()
                                                   : WebSocketError(WebSocketError::Cancelled));
                f->done     = true;
            }
        }
        if (release) endpoint->release_received();
        endpoint->discard_received();
        release_receive_budget();
    }
    void stop(WebSocketError error) {
        bool cancel = false;
        {
            auto f = fields.lock().unwrap();
            if (f->terminal.is_some()) return;
            f->terminal = Some(error.clone());
            f->closing  = true;
            if (f->close_result.is_none())
                f->close_result = Some(error == WebSocketError::Closed
                                           ? WebSocketResult<empty>(Ok(empty {}))
                                           : WebSocketResult<empty>(Err(error.clone())));
            for (auto& item : f->sends) {
                auto send = item->fields.lock().unwrap();
                if (send->result.is_none())
                    send->result = Some(WebSocketResult<empty>(Err(error.clone())));
                if (! send->started) {
                    send->message = None();
                    send->permit  = None();
                }
                item->changed.notify();
            }
            f->sends.clear();
            f->bytes = usize();
            cancel   = error != WebSocketError::Closed;
        }
        if (cancel) endpoint->cancel();
        release_receive_budget();
        changed.notify();
    }
    auto error(liwebsocketo::WebSocketError error) -> WebSocketError {
        auto detail = endpoint->failure();
        return detail.is_some() ? rstd::move(detail).unwrap() : WebSocketError(error);
    }
    void cancel_send(const Arc<WebSocketSendState>& item) {
        bool active = false;
        {
            auto f    = fields.lock().unwrap();
            auto send = item->fields.lock().unwrap();
            if (send->result.is_some()) return;
            active = send->started;
            if (! active) {
                send->result =
                    Some(WebSocketResult<empty>(Err(WebSocketError(WebSocketError::Cancelled))));
                send->message = None();
                send->permit  = None();
                for (usize i {}; i < f->sends.len(); ++i)
                    if (Arc<WebSocketSendState>::ptr_eq(f->sends[i], item)) {
                        f->sends.remove(i);
                        f->bytes -= item->bytes;
                        break;
                    }
            }
        }
        if (active) stop(WebSocketError::Cancelled);
        item->changed.notify();
        changed.notify();
    }
};
struct MessageDriver {
    Arc<WebSocketState> state;
    void                request_stop(lf::StopReason reason) const {
        state->stop(reason == lf::StopReason::Deadline ? WebSocketError::Timeout
                                                       : WebSocketError::Cancelled);
        state->discard_received();
    }
    auto run(lf::TransferContext, lf::OutputSender<empty> output) const
        -> coro<lf::ProtocolCompletion> {
        if (output.publish(empty {}).is_err()) state->stop(WebSocketError::Cancelled);
        for (;;) {
            auto                            event = state->changed.listen();
            Option<Arc<WebSocketSendState>> item;
            Option<Bytes>                   close;
            Option<lf::Instant>             deadline;
            Option<WebSocketError>          terminal;
            {
                auto f   = state->fields.lock().unwrap();
                terminal = f->terminal.clone();
                deadline = f->close_deadline;
                if (terminal.is_none()) {
                    if (! f->sends.is_empty()) {
                        item                                     = Some(f->sends[usize()].clone());
                        (*item)->fields.lock().unwrap()->started = true;
                    } else if (f->closing)
                        close = Some(f->close_payload.clone());
                }
            }
            if (terminal.is_some()) break;
            if (deadline.is_some() && lf::Instant::now() >= *deadline) {
                state->stop(WebSocketError::Timeout);
                break;
            }
            if (item.is_some()) {
                auto message = (*item)->fields.lock().unwrap()->message.take().unwrap();
                auto writing = state->endpoint->send(rstd::move(message));
                Option<liwebsocketo::WebSocketResult<empty>> result;
                for (;;) {
                    auto event = state->changed.listen();
                    {
                        auto f   = state->fields.lock().unwrap();
                        deadline = f->close_deadline;
                    }
                    auto progress =
                        rstd::async::select(rstd::future::by_ref(writing), rstd::move(event));
                    if (deadline.is_some()) {
                        auto done = co_await rstd::async::timeout(rstd::move(progress),
                                                                  lf::remaining(*deadline));
                        if (done.is_err()) {
                            state->stop(WebSocketError::Timeout);
                            break;
                        }
                        if (done->is_right()) continue;
                        auto value = rstd::move(done).unwrap().unwrap_left();
                        if (value.is_err())
                            state->stop(WebSocketError::Cancelled);
                        else
                            result = Some(rstd::move(value).unwrap());
                    } else {
                        auto done = co_await rstd::move(progress);
                        if (done.is_right()) continue;
                        auto value = rstd::move(done).unwrap_left();
                        if (value.is_err())
                            state->stop(WebSocketError::Cancelled);
                        else
                            result = Some(rstd::move(value).unwrap());
                    }
                    break;
                }
                if (result.is_some() && result->is_err())
                    state->stop(state->error(result->unwrap_err()));
                bool stopped;
                {
                    auto f    = state->fields.lock().unwrap();
                    stopped   = f->terminal.is_some();
                    auto send = (*item)->fields.lock().unwrap();
                    if (send->result.is_none())
                        send->result = Some(WebSocketResult<empty>(Ok(empty {})));
                    for (usize i {}; i < f->sends.len(); ++i)
                        if (Arc<WebSocketSendState>::ptr_eq(f->sends[i], *item)) {
                            f->sends.remove(i);
                            f->bytes -= (*item)->bytes;
                            break;
                        }
                }
                if (stopped) (void)co_await state->endpoint->shutdown();
                (*item)->fields.lock().unwrap()->permit = None();
                (*item)->changed.notify();
                continue;
            }
            if (close.is_some()) {
                auto closing = state->endpoint->close(rstd::move(close).unwrap());
                auto done =
                    co_await rstd::async::timeout(rstd::move(closing), lf::remaining(*deadline));
                if (done.is_err())
                    state->stop(WebSocketError::Timeout);
                else if (done->is_err())
                    state->stop(WebSocketError::Cancelled);
                else if ((*done)->is_err())
                    state->stop(state->error((*done)->unwrap_err()));
                else
                    state->stop(WebSocketError::Closed);
                break;
            }
            auto done = co_await rstd::async::select(
                rstd::move(event), state->endpoint->completion().wait().into_future());
            if (done.is_right()) {
                auto report  = rstd::move(done).unwrap_right();
                auto failure = state->endpoint->failure();
                if (failure.is_some())
                    state->stop(rstd::move(failure).unwrap());
                else if (report->logical.result.is_ok())
                    state->stop(WebSocketError::Closed);
                else
                    state->stop(
                        WebSocketError(WebSocketError::Transport)
                            .with_cause(report->logical.result.as_ref().unwrap_err().clone()));
            }
        }
        auto error = state->fields.lock().unwrap()->terminal->clone();
        co_return lf::ProtocolCompletion { error == WebSocketError::Closed
                                               ? lf::Result<empty>(Ok(empty {}))
                                               : lf::Result<empty>(
                                                     Err(websocket_transport_error(error))),
                                           lf::RemoteOutcome::Unknown };
    }
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        auto result = co_await state->endpoint->shutdown();
        state->release_receive_budget();
        co_return rstd::move(result);
    }
};
PendingSend::PendingSend(Arc<WebSocketState> owner, Arc<WebSocketSendState> state)
    : owner_(rstd::move(owner)), state_(rstd::move(state)) {}
PendingSend::PendingSend(PendingSend&& other) noexcept
    : owner_(rstd::move(other.owner_)),
      state_(rstd::move(other.state_)),
      owned_(rstd::exchange(other.owned_, false)) {}
auto PendingSend::operator=(PendingSend&& other) noexcept -> PendingSend& {
    if (this != &other) {
        cancel();
        owner_ = rstd::move(other.owner_);
        state_ = rstd::move(other.state_);
        owned_ = rstd::exchange(other.owned_, false);
    }
    return *this;
}
PendingSend::~PendingSend() { cancel(); }
void PendingSend::cancel() const {
    if (owned_) owner_->cancel_send(state_);
}
auto wait_written(Arc<WebSocketSendState> state) -> coro<WebSocketResult<empty>> {
    for (;;) {
        auto event = state->changed.listen();
        {
            auto f = state->fields.lock().unwrap();
            if (f->result.is_some()) {
                if (f->result->is_err()) co_return Err(f->result->as_ref().unwrap_err().clone());
                co_return Ok(empty {});
            }
        }
        co_await rstd::move(event);
    }
}
auto PendingSend::written() const -> coro<WebSocketResult<empty>> {
    return wait_written(state_.clone());
}
struct ReceiveWait {
    using Output = WebSocketResult<liwebsocketo::Message>;
    Arc<WebSocketState>        owner;
    Arc<WebSocketReceiveState> state;
    auto poll(mut_ref<ReceiveWait> self, rstd::task::Context& cx) -> rstd::task::Poll<Output> {
        using Poll = rstd::task::Poll<Output>;
        Option<WebSocketError> failure;
        {
            auto f = self->state->fields.lock().unwrap();
            if (f->rejected.is_some()) {
                f->done = true;
                return Poll::Ready(Err(f->rejected->clone()));
            }
            if (f->done) return Poll::Ready(Err(WebSocketError(WebSocketError::Closed)));
            auto ready = rstd::future::poll(*f->pending, cx);
            if (ready.is_pending()) return Poll::Pending();
            auto received = rstd::move(ready).take();
            f->pending    = None();
            f->done       = true;
            if (received.is_err())
                failure = Some(WebSocketError(WebSocketError::Cancelled));
            else {
                auto value = rstd::move(received).unwrap();
                if (value.is_err())
                    failure = Some(self->owner->error(value.unwrap_err()));
                else {
                    f->delivered = true;
                    return Poll::Ready(Ok(rstd::move(value).unwrap()));
                }
            }
        }
        self->owner->stop(failure->clone());
        return Poll::Ready(Err(failure.take().unwrap()));
    }
};
struct ReceiveGuard {
    Arc<WebSocketReceiveState> state;
    ~ReceiveGuard() { state->fields.lock().unwrap()->reading = false; }
};
auto wait_message(Arc<WebSocketState> owner, Arc<WebSocketReceiveState> state)
    -> coro<WebSocketResult<liwebsocketo::Message>> {
    {
        auto f = state->fields.lock().unwrap();
        if (f->reading) co_return Err(WebSocketError(WebSocketError::Busy));
        f->reading = true;
    }
    ReceiveGuard guard { state.clone() };
    co_return co_await ReceiveWait { rstd::move(owner), rstd::move(state) };
}
PendingReceive::PendingReceive(Arc<WebSocketState> owner, Arc<WebSocketReceiveState> state)
    : owner_(rstd::move(owner)), state_(rstd::move(state)) {}
PendingReceive::PendingReceive(PendingReceive&& other) noexcept
    : owner_(rstd::move(other.owner_)),
      state_(rstd::move(other.state_)),
      owned_(rstd::exchange(other.owned_, false)) {}
void release_receive(const Arc<WebSocketState>& owner, const Arc<WebSocketReceiveState>& state) {
    bool cancel, release;
    {
        auto f  = state->fields.lock().unwrap();
        cancel  = ! f->done;
        release = rstd::exchange(f->delivered, false);
    }
    if (cancel) {
        owner->stop(WebSocketError::Cancelled);
        owner->discard_received();
    }
    if (release) owner->endpoint->release_received();
    {
        auto f = owner->fields.lock().unwrap();
        if (f->receiver.is_some() && Arc<WebSocketReceiveState>::ptr_eq(*f->receiver, state))
            f->receiver = None();
    }
    owner->release_receive_budget();
}
auto PendingReceive::operator=(PendingReceive&& other) noexcept -> PendingReceive& {
    if (this != &other) {
        if (owned_) release_receive(owner_, state_);
        owner_ = rstd::move(other.owner_);
        state_ = rstd::move(other.state_);
        owned_ = rstd::exchange(other.owned_, false);
    }
    return *this;
}
PendingReceive::~PendingReceive() {
    if (owned_) release_receive(owner_, state_);
}
auto PendingReceive::message() const -> coro<WebSocketResult<liwebsocketo::Message>> {
    return wait_message(owner_.clone(), state_.clone());
}
void PendingReceive::cancel() const {
    if (owned_) {
        {
            auto f = state_->fields.lock().unwrap();
            if (f->done) return;
        }
        owner_->stop(WebSocketError::Cancelled);
        owner_->discard_received();
    }
}
auto receive_from(const Arc<WebSocketState>& state, bool stream) -> PendingReceive;
WebSocketConnection::WebSocketConnection(Arc<WebSocketState> state, lf::Transfer<empty> transfer)
    : state_(rstd::move(state)), transfer_(rstd::move(transfer)) {}
WebSocketConnection::WebSocketConnection(WebSocketConnection&&) noexcept = default;
auto WebSocketConnection::operator=(WebSocketConnection&& other) noexcept -> WebSocketConnection& {
    if (this != &other) {
        if (state_) cancel();
        state_    = rstd::move(other.state_);
        transfer_ = rstd::move(other.transfer_);
    }
    return *this;
}
WebSocketConnection::~WebSocketConnection() {
    if (state_) cancel();
}
auto WebSocketConnection::connect(Arc<Session> session, DuplexRequest target,
                                  lihttpto::Headers headers, Vec<String> protocols,
                                  WebSocketOptions options)
    -> coro<rstd::Result<WebSocketConnection, WebSocketConnectFailure>> {
    if (! session || options.send_messages == usize() || options.send_bytes == usize() ||
        options.close_timeout.is_zero() || options.receive_messages == usize() ||
        options.receive_bytes == usize())
        co_return Err(WebSocketConnectFailure {
            .admission = Some(lf::Error::InvalidOptions("invalid websocket limits"_Str)) });
    auto bound = options.receive_bytes.checked_add(options.limits.message_bytes);
    if (bound.is_none() ||
        bound
            ->checked_add((options.limits.frame_bytes > usize(65536) ? options.limits.frame_bytes
                                                                     : usize(65536)))
            .is_none() ||
        options.limits.frame_bytes == usize() || options.limits.message_bytes == usize())
        co_return Err(WebSocketConnectFailure {
            .admission = Some(lf::Error::InvalidOptions("invalid websocket memory limits"_Str)) });
    auto resources = session->resources();
    auto receive_budget =
        resources.try_reserve(lf::ResourceAmount { .bytes = options.receive_bytes });
    if (receive_budget.is_err())
        co_return Err(
            WebSocketConnectFailure { .admission = Some(rstd::move(receive_budget).unwrap_err()) });
    auto                                      backend = session->websocket_backend();
    Option<Arc<rstd::dyn<WebSocketEndpoint>>> endpoint;
    if (backend.is_some()) {
        auto plan = session->prepare_connection(rstd::move(target), options.upgrade.duplex);
        if (plan.is_err())
            co_return Err(
                WebSocketConnectFailure { .admission = Some(rstd::move(plan).unwrap_err().error) });
        auto opened = co_await (*backend)->connect(resources.clone(),
                                                   rstd::move(plan).unwrap(),
                                                   rstd::move(headers),
                                                   rstd::move(protocols),
                                                   options);
        if (opened.is_err()) co_return Err(rstd::move(opened).unwrap_err());
        endpoint = Some(rstd::move(opened).unwrap());
    } else {
        auto opened = co_await connect_native_websocket(session.clone(),
                                                        rstd::move(target),
                                                        rstd::move(headers),
                                                        rstd::move(protocols),
                                                        options);
        if (opened.is_err()) co_return Err(rstd::move(opened).unwrap_err());
        endpoint = Some(rstd::move(opened).unwrap());
    }
    auto state = Arc<WebSocketState>::make(
        endpoint.take().unwrap(), resources.clone(), options, rstd::move(receive_budget).unwrap());
    auto submitted = resources.handle().submit(
        lf::PreparedOperation<empty>::make(MessageDriver { state.clone() }));
    if (submitted.is_err()) {
        auto cleanup = co_await state->endpoint->shutdown();
        auto report  = co_await state->endpoint->completion().wait();
        co_return Err(WebSocketConnectFailure {
            .admission = Some(rstd::move(submitted).unwrap_err().error),
            .report    = Some(rstd::move(report)),
            .transport =
                cleanup.is_err() ? Some(rstd::move(cleanup).unwrap_err()) : None<lf::Error>() });
    }
    auto transfer = rstd::move(submitted).unwrap();
    auto ready    = co_await transfer.output();
    if (ready.is_err())
        co_return Err(
            WebSocketConnectFailure { .report    = Some(co_await transfer.control().close()),
                                      .transport = Some(rstd::move(ready).unwrap_err()) });
    co_return Ok(WebSocketConnection(rstd::move(state), rstd::move(transfer)));
}
auto WebSocketConnection::protocol() const -> Option<String> {
    return state_->endpoint->protocol();
}
auto WebSocketConnection::send(liwebsocketo::Message message) const
    -> rstd::Result<PendingSend, RejectedMessage> {
    Option<Arc<WebSocketSendState>> accepted;
    {
        auto f      = state_->fields.lock().unwrap();
        auto reject = [&](WebSocketError error) -> rstd::Result<PendingSend, RejectedMessage> {
            return Err(RejectedMessage { rstd::move(error), rstd::move(message) });
        };
        if (f->terminal.is_some()) return reject(f->terminal->clone());
        if (f->closing) return reject(WebSocketError::Closed);
        if (message.kind == liwebsocketo::Kind::Pong || message.kind == liwebsocketo::Kind::Close)
            return reject(WebSocketError::Protocol);
        if (message.data.len() > state_->options.limits.frame_bytes)
            return reject(WebSocketError(WebSocketError::FrameTooLarge,
                                         state_->options.limits.frame_bytes,
                                         message.data.len()));
        if (message.kind != liwebsocketo::Kind::Ping &&
            message.data.len() > state_->options.limits.message_bytes)
            return reject(WebSocketError(WebSocketError::MessageTooLarge,
                                         state_->options.limits.message_bytes,
                                         message.data.len()));
        if (liwebsocketo::validate_message(message, state_->options.limits).is_err())
            return reject(WebSocketError::Protocol);
        if (f->sends.len() >= state_->options.send_messages)
            return reject(WebSocketError(WebSocketError::QueueFull,
                                         state_->options.send_messages,
                                         f->sends.len() + usize(1)));
        if (message.data.len() > state_->options.send_bytes - f->bytes)
            return reject(WebSocketError(WebSocketError::QueueFull,
                                         state_->options.send_bytes,
                                         f->bytes + message.data.len()));
        auto permit = state_->resources.try_reserve(
            lf::ResourceAmount { .resources = usize(1), .bytes = message.data.len() });
        if (permit.is_err())
            return reject(WebSocketError(WebSocketError::QueueFull)
                              .with_cause(rstd::move(permit).unwrap_err()));
        auto item = Arc<WebSocketSendState>::make(rstd::move(message), rstd::move(permit).unwrap());
        f->bytes += item->bytes;
        f->sends.push(item.clone());
        accepted = Some(rstd::move(item));
    }
    state_->changed.notify();
    return Ok(PendingSend(state_.clone(), accepted.take().unwrap()));
}
auto receive_from(const Arc<WebSocketState>& state, bool stream) -> PendingReceive {
    auto receiving = Arc<WebSocketReceiveState>::make();
    auto f         = state->fields.lock().unwrap();
    if (f->receiver.is_some() || (! stream && f->stream)) {
        auto r      = receiving->fields.lock().unwrap();
        r->rejected = Some(WebSocketError(WebSocketError::Busy));
        r->done     = true;
    } else {
        receiving->fields.lock().unwrap()->pending = Some(state->endpoint->receive());
        f->receiver                                = Some(receiving.clone());
    }
    return PendingReceive(state.clone(), rstd::move(receiving));
}
auto WebSocketConnection::receive() const -> PendingReceive { return receive_from(state_, false); }
auto WebSocketConnection::messages() const -> WebSocketResult<MessageStream> {
    auto f = state_->fields.lock().unwrap();
    if (f->stream || f->receiver.is_some()) return Err(WebSocketError(WebSocketError::Busy));
    f->stream = true;
    return Ok(MessageStream(state_.clone()));
}
auto request_connection_close(const Arc<WebSocketState>& state, Bytes payload)
    -> WebSocketResult<empty> {
    if (liwebsocketo::validate_message({ liwebsocketo::Kind::Close, payload.clone() },
                                       state->options.limits)
            .is_err())
        return Err(WebSocketError(WebSocketError::Protocol));
    {
        auto f = state->fields.lock().unwrap();
        if (! f->closing) {
            f->closing        = true;
            f->close_payload  = rstd::move(payload);
            f->close_deadline = Some(lf::Instant::now() + state->options.close_timeout);
        }
    }
    state->changed.notify();
    return Ok(empty {});
}
auto close_connection(Arc<WebSocketState> state, Bytes payload) -> coro<WebSocketResult<empty>> {
    auto request = request_connection_close(state, rstd::move(payload));
    if (request.is_err()) co_return Err(rstd::move(request).unwrap_err());
    for (;;) {
        auto event = state->changed.listen();
        {
            auto f = state->fields.lock().unwrap();
            if (f->close_result.is_some()) {
                if (f->close_result->is_err())
                    co_return Err(f->close_result->as_ref().unwrap_err().clone());
                co_return Ok(empty {});
            }
        }
        co_await rstd::move(event);
    }
}
auto WebSocketConnection::request_close(Bytes payload) const -> WebSocketResult<empty> {
    return request_connection_close(state_, rstd::move(payload));
}
auto WebSocketConnection::close(Bytes payload) const -> coro<WebSocketResult<empty>> {
    return close_connection(state_.clone(), rstd::move(payload));
}
void WebSocketConnection::fail(WebSocketError error) const { state_->stop(rstd::move(error)); }
void WebSocketConnection::cancel() const {
    state_->stop(WebSocketError::Cancelled);
    state_->discard_received();
}
auto WebSocketConnection::completion() const -> lf::TransferControl { return transfer_.control(); }
auto shutdown_connection(Arc<WebSocketState> state, lf::TransferControl control)
    -> coro<lf::Result<empty>> {
    state->stop(WebSocketError::Cancelled);
    state->discard_received();
    auto report = co_await control.wait();
    if (report->cleanup.is_err()) co_return Err(report->cleanup.errors[usize()].clone());
    co_return Ok(empty {});
}
auto WebSocketConnection::shutdown() const -> coro<lf::Result<empty>> {
    return shutdown_connection(state_.clone(), transfer_.control());
}
auto WebSocketConnection::failure() const -> Option<WebSocketError> {
    return state_->fields.lock().unwrap()->terminal.clone();
}
WebSocketConnection::MessageStream::MessageStream(Arc<WebSocketState> state)
    : state_(rstd::move(state)) {}
void WebSocketConnection::MessageStream::release() {
    if (! owned_) return;
    owned_                                 = false;
    waiting_                               = None();
    pending_                               = None();
    state_->fields.lock().unwrap()->stream = false;
}
WebSocketConnection::MessageStream::MessageStream(MessageStream&& other) noexcept
    : state_(rstd::move(other.state_)),
      pending_(other.pending_.take()),
      waiting_(other.waiting_.take()),
      done_(other.done_),
      owned_(rstd::exchange(other.owned_, false)) {}
auto WebSocketConnection::MessageStream::operator=(MessageStream&& other) noexcept
    -> MessageStream& {
    if (this != &other) {
        release();
        state_   = rstd::move(other.state_);
        pending_ = other.pending_.take();
        waiting_ = other.waiting_.take();
        done_    = other.done_;
        owned_   = rstd::exchange(other.owned_, false);
    }
    return *this;
}
WebSocketConnection::MessageStream::~MessageStream() { release(); }
auto WebSocketConnection::MessageStream::poll_next(mut_ref<MessageStream> self,
                                                   rstd::task::Context&   cx)
    -> rstd::task::Poll<Option<Item>> {
    using Poll = rstd::task::Poll<Option<Item>>;
    if (self->done_) return Poll::Ready(None());
    if (self->pending_.is_none()) {
        self->pending_ = Some(receive_from(self->state_, true));
        self->waiting_ = Some(self->pending_->message().into_future());
    }
    auto ready = rstd::future::poll(*self->waiting_, cx);
    if (ready.is_pending()) return Poll::Pending();
    auto value     = rstd::move(ready).take();
    self->waiting_ = None();
    self->pending_ = None();
    if (value.is_err()) {
        self->done_ = true;
        if (value.as_ref().unwrap_err() == WebSocketError::Closed) return Poll::Ready(None());
    }
    return Poll::Ready(Some(rstd::move(value)));
}
} // namespace ncrequest
