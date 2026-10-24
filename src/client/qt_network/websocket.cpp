module;
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
#include <QNetworkRequest>
#include <QHttpHeaders>
#include <QPointer>
#include <QSslError>
#include <QTimer>
module ncrequest;
import :client.qt_network.websocket;
import :client.qt_network.backend;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client::qt_network
{
namespace lf         = lifetchto;
namespace h          = lifetchto::http;
using WebSocketError = liwebsocketo::WebSocketError;
template<class T>
using WebSocketResult = liwebsocketo::WebSocketResult<T>;
using rstd::async::Completion;
using rstd::async::CompletionHandle;
struct WsSend {
    liwebsocketo::Message                    message;
    CompletionHandle<WebSocketResult<empty>> waiter;
};
struct WsFields {
    bool                              enqueued {}, connected {}, done {}, closing_native {};
    Option<WebSocketError>            terminal;
    Option<lf::Error>                 cause;
    Option<String>                    protocol;
    Vec<liwebsocketo::Message>        incoming;
    usize                             bytes {}, retained_bytes {}, retained_messages {};
    Option<ncrequest::WebSocketError> failure;
    Option<CompletionHandle<WebSocketResult<liwebsocketo::Message>>> receiver;
    Option<WsSend>                                                   sending;
    Option<CompletionHandle<WebSocketResult<empty>>>                 closing;
    Bytes                                                            close_payload;
};
auto ws_error(WebSocketError error) -> lf::Error {
    if (error == WebSocketError::Timeout) return lf::Error::Timeout();
    if (error == WebSocketError::Overflow) return lf::Error::TooLarge();
    if (error == WebSocketError::Cancelled) return lf::Error::Canceled();
    if (error == WebSocketError::Protocol)
        return h::protocol_error("Qt WebSocket protocol failure"_Str);
    return lf::Error::Closed();
}
struct WsState {
    h::ConnectionPlan           plan;
    lihttpto::Headers           headers;
    Vec<String>                 protocols;
    WebSocketOptions            options;
    usize                       window;
    lf::Instant                 handshake_deadline;
    Option<lf::ResourcePermit>  native_buffer;
    rstd::sync::Mutex<WsFields> fields { WsFields {} };
    lf::Event                   changed;
    Dispatch                    dispatch;
    WsState(h::ConnectionPlan plan, lihttpto::Headers headers, Vec<String> protocols,
            WebSocketOptions options, Dispatch dispatch)
        : plan(rstd::move(plan)),
          headers(rstd::move(headers)),
          protocols(rstd::move(protocols)),
          options(options),
          window(options.limits.frame_bytes < usize(65536) ? usize(65536)
                                                           : options.limits.frame_bytes),
          handshake_deadline(lf::Instant::now() + options.upgrade.handshake_timeout),
          dispatch(rstd::move(dispatch)) {}
    void notify() {
        changed.notify();
        dispatch.notify();
    }
    void finish_locked(WsFields& f, WebSocketError error, Option<lf::Error> cause = {}) {
        if (f.terminal.is_some()) return;
        f.terminal = Some(error);
        if (cause.is_some()) f.cause = rstd::move(cause);
        if (error != WebSocketError::Closed && ! f.cause.is_some()) f.cause = Some(ws_error(error));
        if (f.receiver.is_some()) (void)f.receiver.take()->complete(Err(error));
        if (f.sending.is_some()) (void)f.sending.take()->waiter.complete(Err(error));
        if (f.closing.is_some())
            (void)f.closing.take()->complete(error == WebSocketError::Closed
                                                 ? WebSocketResult<empty>(Ok(empty {}))
                                                 : WebSocketResult<empty>(Err(error)));
        if (error != WebSocketError::Closed) {
            f.incoming.clear();
            f.bytes = usize();
        }
    }
    void fail(WebSocketError error, Option<lf::Error> cause = {}) {
        {
            auto f = fields.lock().unwrap();
            finish_locked(*f, error, rstd::move(cause));
        }
        notify();
    }
    void complete() {
        {
            auto f  = fields.lock().unwrap();
            f->done = true;
        }
        notify();
    }
    void receive(liwebsocketo::Message message) {
        {
            auto f = fields.lock().unwrap();
            if (f->terminal.is_some() ||
                (f->closing.is_some() && message.kind != liwebsocketo::Kind::Pong))
                return;
            if ((message.kind == liwebsocketo::Kind::Text ||
                 message.kind == liwebsocketo::Kind::Binary) &&
                message.data.len() > options.limits.message_bytes) {
                f->failure =
                    Some(ncrequest::WebSocketError(ncrequest::WebSocketError::MessageTooLarge,
                                                   options.limits.message_bytes,
                                                   message.data.len()));
                finish_locked(*f, WebSocketError::Overflow);
            } else {
                bool count = f->incoming.len() + f->retained_messages >= options.receive_messages;
                bool bytes = message.data.len() > options.receive_bytes - f->bytes;
                if (count || bytes) {
                    f->failure = Some(ncrequest::WebSocketError(
                        count ? ncrequest::WebSocketError::ReceiveMessagesFull
                              : ncrequest::WebSocketError::ReceiveBytesFull,
                        count ? options.receive_messages : options.receive_bytes,
                        count ? f->incoming.len() + f->retained_messages + usize(1)
                              : f->bytes + message.data.len()));
                    finish_locked(*f, WebSocketError::Overflow);
                } else if (f->receiver.is_some()) {
                    f->bytes += message.data.len();
                    f->retained_bytes += message.data.len();
                    ++f->retained_messages;
                    if (f->receiver.take()->complete(Ok(rstd::move(message))).is_err())
                        finish_locked(*f, WebSocketError::Cancelled);
                } else {
                    f->bytes += message.data.len();
                    f->incoming.push(rstd::move(message));
                }
            }
        }
        notify();
    }
};
struct WsActive {
    Arc<WsState>         state;
    QPointer<QWebSocket> socket;
    bool                 writing {}, closing {};
    Option<lf::Instant>  closing_deadline;
    QTimer*              timer {};
    explicit WsActive(Arc<WsState> state): state(rstd::move(state)) {}
};
struct WsServiceFields {
    bool              closed {};
    Vec<Arc<WsState>> queued;
};
struct WebSocketServiceState {
    QSslConfiguration                  ssl;
    QNetworkProxy                      proxy;
    h::TlsVerification                 verification;
    Dispatch                           dispatch;
    rstd::sync::Mutex<WsServiceFields> fields { WsServiceFields {} };
    Vec<Box<WsActive>>                 active;
    WebSocketServiceState(QSslConfiguration ssl, QNetworkProxy proxy,
                          h::TlsVerification verification, Dispatch dispatch)
        : ssl(rstd::move(ssl)),
          proxy(rstd::move(proxy)),
          verification(verification),
          dispatch(rstd::move(dispatch)) {}
};
auto ws_bytes(const Bytes& bytes) -> QByteArray {
    return QByteArray(reinterpret_cast<const char*>(bytes.as_slice().as_raw_ptr()),
                      qsizetype(bytes.len().to_primitive()));
}
auto ws_text(const QString& text) -> String {
    auto encoded = text.toUtf8();
    return String::make(
        rstd::str_::from_utf8(
            slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(encoded.constData()),
                                      usize(encoded.size())))
            .unwrap());
}
auto ws_native_error(QWebSocket* socket) -> lf::Error {
    return lf::Error::Extension(
        "qt websocket"_Str,
        Arc<rstd::dyn<rstd::error::Error>>::make(WebSocketNativeError(
            i32(socket->error()), u16(socket->closeCode()), ws_text(socket->errorString()))));
}
void ws_dispose(WsActive& active) {
    if (active.socket) {
        QObject::disconnect(active.socket, nullptr, nullptr, nullptr);
        active.socket->abort();
        delete active.socket.data();
    }
    active.state->complete();
}
struct WsDriver {
    Arc<WebSocketServiceState> service;
    Arc<WsState>               state;
    void                       request_stop(lf::StopReason reason) const {
        state->fail(reason == lf::StopReason::Deadline ? WebSocketError::Timeout
                                                       : WebSocketError::Cancelled);
    }
    auto run(lf::TransferContext context, lf::OutputSender<empty> output) const
        -> coro<lf::ProtocolCompletion> {
        auto attempt = context.begin_attempt();
        if (attempt.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(attempt).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        auto permit = co_await context.resources().reserve(
            lf::ResourceAmount { .bytes = state->window + state->options.limits.message_bytes });
        if (permit.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(permit).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        state->native_buffer = Some(rstd::move(permit).unwrap());
        bool queued          = false;
        {
            auto f = service->fields.lock().unwrap();
            if (! f->closed) {
                state->fields.lock().unwrap()->enqueued = true;
                f->queued.push(state.clone());
                queued = true;
            }
        }
        if (! queued) {
            state->fail(WebSocketError::Cancelled);
            state->complete();
        }
        state->notify();
        bool published = false;
        for (;;) {
            auto event = state->changed.listen();
            bool connected, terminal;
            {
                auto f    = state->fields.lock().unwrap();
                connected = f->connected;
                terminal  = f->terminal.is_some();
            }
            if (connected && ! published) {
                published = true;
                if (output.publish(empty {}).is_err()) state->fail(WebSocketError::Cancelled);
            }
            if (terminal) break;
            co_await rstd::move(event);
        }
        auto f = state->fields.lock().unwrap();
        co_return lf::ProtocolCompletion {
            f->terminal == Some(WebSocketError::Closed)
                ? lf::Result<empty>(Ok(empty {}))
                : lf::Result<empty>(
                      Err(f->cause.is_some() ? f->cause->clone() : ws_error(*f->terminal))),
            queued ? lf::RemoteOutcome::Unknown : lf::RemoteOutcome::NotDispatched
        };
    }
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        {
            auto f = state->fields.lock().unwrap();
            if (! f->enqueued) f->done = true;
        }
        for (;;) {
            auto event = state->changed.listen();
            if (state->fields.lock().unwrap()->done) break;
            co_await rstd::move(event);
        }
        state->native_buffer = None();
        co_return Ok(empty {});
    }
};
template<class T>
auto ws_rejected(WebSocketError error) -> Completion<WebSocketResult<T>> {
    auto pair = Completion<WebSocketResult<T>>::make().unwrap();
    (void)pair.template get<1>().complete(Err(error));
    return rstd::move(pair.template get<0>());
}
struct WsNotify {
    Arc<WsState> state;
    ~WsNotify() { state->notify(); }
};
struct QtEndpoint {
    lf::Transfer<empty> transfer;
    Arc<WsState>        state;
    QtEndpoint(lf::Transfer<empty> transfer, Arc<WsState> state)
        : transfer(rstd::move(transfer)), state(rstd::move(state)) {}
    auto protocol() const -> Option<String> {
        return state->fields.lock().unwrap()->protocol.clone();
    }
    auto send(liwebsocketo::Message message) const -> Completion<WebSocketResult<empty>> {
        WsNotify wake { state.clone() };
        auto     f = state->fields.lock().unwrap();
        if (f->terminal.is_some()) return ws_rejected<empty>(*f->terminal);
        if (f->closing.is_some() || f->closing_native)
            return ws_rejected<empty>(WebSocketError::Closed);
        if (f->sending.is_some()) return ws_rejected<empty>(WebSocketError::Busy);
        if (message.kind == liwebsocketo::Kind::Close || message.kind == liwebsocketo::Kind::Pong)
            return ws_rejected<empty>(WebSocketError::Protocol);
        if (liwebsocketo::validate_message(message, state->options.limits).is_err())
            return ws_rejected<empty>(WebSocketError::Protocol);
        auto pair  = Completion<WebSocketResult<empty>>::make().unwrap();
        f->sending = Some(WsSend { rstd::move(message), rstd::move(pair.template get<1>()) });
        return rstd::move(pair.template get<0>());
    }
    auto receive() const -> Completion<WebSocketResult<liwebsocketo::Message>> {
        auto f = state->fields.lock().unwrap();
        if (f->receiver.is_some() || f->retained_messages > usize())
            return ws_rejected<liwebsocketo::Message>(WebSocketError::Busy);
        if (f->incoming.is_empty() && f->terminal.is_some())
            return ws_rejected<liwebsocketo::Message>(*f->terminal);
        auto pair = Completion<WebSocketResult<liwebsocketo::Message>>::make().unwrap();
        if (f->incoming.is_empty())
            f->receiver = Some(rstd::move(pair.template get<1>()));
        else {
            auto message = f->incoming.remove(usize());
            f->retained_bytes += message.data.len();
            ++f->retained_messages;
            (void)pair.template get<1>().complete(Ok(rstd::move(message)));
        }
        return rstd::move(pair.template get<0>());
    }
    auto close(Bytes payload) const -> Completion<WebSocketResult<empty>> {
        WsNotify wake { state.clone() };
        auto     f = state->fields.lock().unwrap();
        if (f->terminal.is_some()) return ws_rejected<empty>(*f->terminal);
        if (f->closing.is_some()) return ws_rejected<empty>(WebSocketError::Busy);
        if (liwebsocketo::validate_message({ liwebsocketo::Kind::Close, payload.clone() },
                                           state->options.limits)
                .is_err())
            return ws_rejected<empty>(WebSocketError::Protocol);
        auto pair        = Completion<WebSocketResult<empty>>::make().unwrap();
        f->close_payload = rstd::move(payload);
        f->closing       = Some(rstd::move(pair.template get<1>()));
        return rstd::move(pair.template get<0>());
    }
    void cancel() const {
        state->fail(WebSocketError::Cancelled);
        transfer.control().cancel();
    }
    auto completion() const -> lf::TransferControl { return transfer.control(); }
    auto has_receiver() const -> bool { return state->fields.lock().unwrap()->receiver.is_some(); }
    auto has_messages() const -> bool {
        auto f = state->fields.lock().unwrap();
        return ! f->incoming.is_empty() || f->retained_messages > usize();
    }
    void discard_received() const {
        auto f = state->fields.lock().unwrap();
        f->incoming.clear();
        f->bytes = f->retained_bytes;
    }
    void release_received() const {
        auto f            = state->fields.lock().unwrap();
        f->bytes          = f->bytes >= f->retained_bytes ? f->bytes - f->retained_bytes : usize();
        f->retained_bytes = {};
        f->retained_messages = {};
    }
    auto failure() const -> Option<ncrequest::WebSocketError> {
        auto f = state->fields.lock().unwrap();
        if (f->failure.is_some()) return Some(f->failure->clone());
        if (f->terminal.is_none()) return None();
        if (*f->terminal == WebSocketError::Overflow) {
            auto error = ncrequest::WebSocketError(ncrequest::WebSocketError::IncomingLimit);
            return f->cause.is_some() ? Some(rstd::move(error).with_cause(f->cause->clone()))
                                      : Some(rstd::move(error));
        }
        auto error = ncrequest::WebSocketError(*f->terminal);
        if (f->cause.is_some()) return Some(rstd::move(error).with_cause(f->cause->clone()));
        return Some(rstd::move(error));
    }
    static auto finish(Arc<WsState> state, lf::TransferControl control) -> coro<lf::Result<empty>> {
        bool closed = [&] {
            auto f = state->fields.lock().unwrap();
            return f->terminal == Some(WebSocketError::Closed) || f->closing_native;
        }();
        auto report = co_await (closed ? control.wait() : control.close());
        if (report->cleanup.is_err()) co_return Err(report->cleanup.errors[usize()].clone());
        co_return Ok(empty {});
    }
    auto shutdown() const -> coro<lf::Result<empty>> {
        return finish(state.clone(), transfer.control());
    }
};
struct QtMessageBackend {
    Arc<WebSocketServiceState> state;
    auto                       capabilities() const -> WebSocketCapabilities {
        return { .supported               = true,
                 .receive_limit_details   = false,
                 .frame_sized_read_window = true };
    }
    auto connect(lf::ResourceContext, h::ConnectionPlan, lihttpto::Headers, Vec<String>,
                 WebSocketOptions) const
        -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>>;
};
} // namespace ncrequest::client::qt_network
namespace rstd
{
template<>
struct Impl<ncrequest::WebSocketEndpoint, ncrequest::client::qt_network::QtEndpoint>
    : ImplBase<ncrequest::client::qt_network::QtEndpoint> {
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
template<>
struct Impl<ncrequest::WebSocketBackend, ncrequest::client::qt_network::QtMessageBackend>
    : ImplBase<ncrequest::client::qt_network::QtMessageBackend> {
    auto capabilities() const { return this->self().capabilities(); }
    auto connect(lifetchto::ResourceContext resources, lifetchto::http::ConnectionPlan plan,
                 lihttpto::Headers headers, ::alloc::vec::Vec<::alloc::string::String> protocols,
                 ncrequest::WebSocketOptions options) const {
        return this->self().connect(rstd::move(resources),
                                    rstd::move(plan),
                                    rstd::move(headers),
                                    rstd::move(protocols),
                                    options);
    }
};
} // namespace rstd
namespace ncrequest::client::qt_network
{
auto connect_qt_websocket(Arc<WebSocketServiceState> state, lf::ResourceContext resources,
                          h::ConnectionPlan plan, lihttpto::Headers headers, Vec<String> protocols,
                          WebSocketOptions options)
    -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>> {
    auto invalid = [](lf::Error error) {
        return WebSocketConnectFailure { .admission = Some(rstd::move(error)) };
    };
    if (state->fields.lock().unwrap()->closed) co_return Err(invalid(lf::Error::Closed()));
    if (plan.request.url.http_origin().is_none() || plan.request.url.userinfo().is_some() ||
        options.receive_messages == usize() || options.receive_bytes == usize() ||
        options.limits.frame_bytes == usize() || options.limits.message_bytes == usize() ||
        options.close_timeout.is_zero() || options.upgrade.handshake_timeout.is_zero() ||
        options.limits.frame_bytes > usize(QWebSocket::maxIncomingFrameSize()) ||
        options.limits.message_bytes > usize(QWebSocket::maxIncomingMessageSize()))
        co_return Err(invalid(lf::Error::InvalidOptions("invalid Qt WebSocket options"_Str)));
    if (plan.request.route.route.is_some())
        co_return Err(
            invalid(lf::Error::Unsupported("QWebSocket does not support explicit routes"_Str)));
    if (options.upgrade.header_bytes != usize(65536) ||
        options.upgrade.error_body_bytes != usize(65536))
        co_return Err(invalid(lf::Error::Unsupported(
            "QWebSocket owns handshake limits and does not expose failure response bodies"_Str)));
    auto valid = liwebsocketo::validate_client_handshake(headers, protocols);
    if (valid.is_err())
        co_return Err(
            WebSocketConnectFailure { .handshake = Some(rstd::move(valid).unwrap_err()) });
    for (const auto& field : headers)
        if (headers.get_all(field.name.as_str()).len() > usize(1))
            co_return Err(invalid(
                lf::Error::Unsupported("QWebSocket cannot preserve repeated request fields"_Str)));
    auto wire      = Arc<WsState>::make(rstd::move(plan),
                                        rstd::move(headers),
                                        rstd::move(protocols),
                                        options,
                                        state->dispatch.operation());
    auto submitted = resources.handle().submit(
        lf::PreparedOperation<empty>::make(WsDriver { state.clone(), wire.clone() }),
        wire->plan.transfer);
    if (submitted.is_err()) co_return Err(invalid(rstd::move(submitted).unwrap_err().error));
    auto transfer  = rstd::move(submitted).unwrap();
    auto connected = co_await transfer.output();
    if (connected.is_err()) {
        auto report = co_await transfer.control().close();
        co_return Err(WebSocketConnectFailure {
            .connection = Some(ncrequest::WebSocketError(
                wire->fields.lock().unwrap()->terminal.unwrap_or(WebSocketError::Transport))),
            .report     = Some(rstd::move(report)),
            .transport  = Some(rstd::move(connected).unwrap_err()) });
    }
    co_return Ok(
        Arc<rstd::dyn<WebSocketEndpoint>>::make(QtEndpoint(rstd::move(transfer), wire.clone())));
}
auto QtMessageBackend::connect(lf::ResourceContext resources, h::ConnectionPlan plan,
                               lihttpto::Headers headers, Vec<String> protocols,
                               WebSocketOptions options) const
    -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>> {
    return connect_qt_websocket(state.clone(),
                                rstd::move(resources),
                                rstd::move(plan),
                                rstd::move(headers),
                                rstd::move(protocols),
                                options);
}
WebSocketService::WebSocketService(QSslConfiguration ssl, QNetworkProxy proxy,
                                   h::TlsVerification verification, Dispatch dispatch)
    : state_(Arc<WebSocketServiceState>::make(rstd::move(ssl), rstd::move(proxy), verification,
                                              rstd::move(dispatch))) {}
WebSocketService::~WebSocketService() = default;
auto WebSocketService::backend() const -> Arc<rstd::dyn<ncrequest::WebSocketBackend>> {
    return Arc<rstd::dyn<ncrequest::WebSocketBackend>>::make(QtMessageBackend { state_.clone() });
}
void WebSocketService::stop() const {
    {
        auto f = state_->fields.lock().unwrap();
        if (f->closed) return;
        f->closed = true;
    }
    state_->dispatch.notify();
}
auto WebSocketService::empty() const -> bool { return state_->active.is_empty(); }
void WebSocketService::dispose() {
    stop();
    Vec<Arc<WsState>> queued;
    {
        auto f = state_->fields.lock().unwrap();
        queued = rstd::move(f->queued);
    }
    for (auto& state : queued) {
        state->fail(WebSocketError::Cancelled);
        state->complete();
    }
    for (auto& active : state_->active) {
        active->state->fail(WebSocketError::Cancelled);
        ws_dispose(*active);
    }
    state_->active.clear();
}
void ws_start(const Arc<WebSocketServiceState>& service, Arc<WsState> state, QObject* owner) {
    bool stopped = state->fields.lock().unwrap()->terminal.is_some();
    if (stopped) {
        state->complete();
        return;
    }
    auto  active   = Box<WsActive>::make(state.clone());
    auto* socket   = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, owner);
    active->socket = socket;
    active->timer  = new QTimer(socket);
    active->timer->setSingleShot(true);
    auto timeout_holder = make_clone_tuple(state.clone());
    QObject::connect(active->timer, &QTimer::timeout, owner, [timeout_holder] {
        timeout_holder.get<0>()->fail(WebSocketError::Timeout);
    });
    active->timer->start(int(
        rstd::min(lf::remaining(state->handshake_deadline).as_millis() + u64(1), u64(2147483647))
            .to_primitive()));
    socket->setProxy(service->proxy);
    socket->setSslConfiguration(service->ssl);
    // Qt 6.8 reads complete frames and handshake lines from the native buffer.
    socket->setReadBufferSize(qint64(state->window.to_primitive()));
    socket->setMaxAllowedIncomingFrameSize(state->options.limits.frame_bytes.to_primitive());
    socket->setMaxAllowedIncomingMessageSize(state->options.limits.message_bytes.to_primitive());
    socket->setOutgoingFrameSize(state->options.limits.frame_bytes.to_primitive());
    auto holder = make_clone_tuple(state.clone());
    QObject::connect(
        socket, &QWebSocket::connected, owner, [holder, socket, timer = active->timer] {
            timer->stop();
            auto state    = holder.get<0>().clone();
            auto protocol = ws_text(socket->subprotocol());
            bool accepted = protocol.is_empty();
            for (const auto& offered : state->protocols) accepted = accepted || protocol == offered;
            if (! accepted) {
                state->fail(WebSocketError::Protocol, Some(ws_native_error(socket)));
                return;
            }
            {
                auto f       = state->fields.lock().unwrap();
                f->connected = true;
                if (! protocol.is_empty()) f->protocol = Some(rstd::move(protocol));
            }
            state->notify();
        });
    QObject::connect(
        socket, &QWebSocket::textMessageReceived, owner, [holder](const QString& value) {
            auto encoded = value.toUtf8();
            holder.get<0>()->receive({ liwebsocketo::Kind::Text,
                                       Bytes::copy_from_slice(slice<u8>::from_raw_parts(
                                           reinterpret_cast<const rstd::byte*>(encoded.constData()),
                                           usize(encoded.size()))) });
        });
    QObject::connect(
        socket, &QWebSocket::binaryMessageReceived, owner, [holder](const QByteArray& value) {
            holder.get<0>()->receive({ liwebsocketo::Kind::Binary,
                                       Bytes::copy_from_slice(slice<u8>::from_raw_parts(
                                           reinterpret_cast<const rstd::byte*>(value.constData()),
                                           usize(value.size()))) });
        });
    QObject::connect(socket, &QWebSocket::pong, owner, [holder](quint64, const QByteArray& value) {
        holder.get<0>()->receive(
            { liwebsocketo::Kind::Pong,
              Bytes::copy_from_slice(slice<u8>::from_raw_parts(
                  reinterpret_cast<const rstd::byte*>(value.constData()), usize(value.size()))) });
    });
    QObject::connect(
        socket, &QWebSocket::aboutToClose, owner, [holder, socket, timer = active->timer] {
            auto state = holder.get<0>().clone();
            {
                auto f            = state->fields.lock().unwrap();
                f->closing_native = true;
            }
            auto code = socket->closeCode();
            if (code == QWebSocketProtocol::CloseCodeTooMuchData)
                state->fail(WebSocketError::Overflow, Some(ws_native_error(socket)));
            else if (code == QWebSocketProtocol::CloseCodeProtocolError ||
                     code == QWebSocketProtocol::CloseCodeWrongDatatype)
                state->fail(WebSocketError::Protocol, Some(ws_native_error(socket)));
            else {
                timer->start(int(
                    rstd::min(state->options.close_timeout.as_millis() + u64(1), u64(2147483647))
                        .to_primitive()));
                state->notify();
            }
        });
    QObject::connect(socket, &QWebSocket::bytesWritten, owner, [holder](qint64) {
        holder.get<0>()->notify();
    });
    QObject::connect(socket,
                     &QWebSocket::errorOccurred,
                     owner,
                     [holder, socket](QAbstractSocket::SocketError error) {
                         auto state = holder.get<0>().clone();
                         auto cause = ws_native_error(socket);
                         if (error == QAbstractSocket::RemoteHostClosedError) {
                             state->fields.lock().unwrap()->cause = Some(rstd::move(cause));
                             return;
                         }
                         state->fail(WebSocketError::Transport, Some(rstd::move(cause)));
                     });
    QObject::connect(socket, &QWebSocket::disconnected, owner, [holder, socket] {
        auto state = holder.get<0>().clone();
        auto code  = socket->closeCode();
        {
            auto f     = state->fields.lock().unwrap();
            auto error = code == QWebSocketProtocol::CloseCodeTooMuchData ? WebSocketError::Overflow
                         : (code == QWebSocketProtocol::CloseCodeProtocolError ||
                            code == QWebSocketProtocol::CloseCodeWrongDatatype)
                             ? WebSocketError::Protocol
                         : (f->cause.is_some() && ! f->closing_native) ? WebSocketError::Transport
                                                                       : WebSocketError::Closed;
            state->finish_locked(*f,
                                 error,
                                 error == WebSocketError::Closed ? None<lf::Error>()
                                                                 : Some(ws_native_error(socket)));
        }
        state->notify();
    });
    if (service->verification == h::TlsVerification::ChainOnly)
        QObject::connect(
            socket, &QWebSocket::sslErrors, owner, [socket](const QList<QSslError>& errors) {
                QList<QSslError> ignored;
                for (const auto& e : errors)
                    if (e.error() == QSslError::HostNameMismatch) ignored.append(e);
                if (! ignored.isEmpty()) socket->ignoreSslErrors(ignored);
            });
    auto url = QUrl::fromEncoded(
        QByteArray(reinterpret_cast<const char*>(state->plan.request.url.as_ref().data()),
                   qsizetype(state->plan.request.url.as_ref().len().to_primitive())));
    url.setScheme(url.scheme() == QStringLiteral("https") ? QStringLiteral("wss")
                                                          : QStringLiteral("ws"));
    url.setFragment(QString());
    QNetworkRequest request(url);
    QHttpHeaders    headers;
    for (const auto& field : state->headers)
        headers.append(
            QByteArray(reinterpret_cast<const char*>(field.name.as_str().data()),
                       qsizetype(field.name.as_str().len().to_primitive())),
            QByteArray(reinterpret_cast<const char*>(field.value.as_slice().as_raw_ptr()),
                       qsizetype(field.value.as_slice().len().to_primitive())));
    request.setHeaders(headers);
    QWebSocketHandshakeOptions options;
    QStringList                protocols;
    for (const auto& value : state->protocols)
        protocols.append(QString::fromUtf8(reinterpret_cast<const char*>(value.as_str().data()),
                                           qsizetype(value.len().to_primitive())));
    options.setSubprotocols(protocols);
    service->active.push(rstd::move(active));
    socket->open(request, options);
}
void ws_pump(WsActive& active) {
    auto  state  = active.state.clone();
    auto* socket = active.socket.data();
    if (! socket) {
        state->fail(WebSocketError::Cancelled);
        state->complete();
        return;
    }
    Option<liwebsocketo::Message> send;
    Option<Bytes>                 close;
    bool                          terminal {};
    {
        auto f = state->fields.lock().unwrap();
        if (! f->connected && lf::Instant::now() >= state->handshake_deadline)
            state->finish_locked(*f, WebSocketError::Timeout);
        if (active.closing_deadline.is_some() && lf::Instant::now() >= *active.closing_deadline)
            state->finish_locked(*f, WebSocketError::Timeout);
        if (f->closing_native && f->sending.is_some())
            (void)f->sending.take()->waiter.complete(Err(WebSocketError::Closed));
        terminal = f->terminal.is_some();
        if (! terminal && f->connected && f->closing.is_some() && ! active.closing) {
            active.closing          = true;
            active.closing_deadline = Some(lf::Instant::now() + state->options.close_timeout);
            close                   = Some(f->close_payload.clone());
            if (f->sending.is_some())
                (void)f->sending.take()->waiter.complete(Err(WebSocketError::Closed));
        } else if (! terminal && f->connected && f->sending.is_some() && ! active.writing) {
            active.writing = true;
            send           = Some(liwebsocketo::Message { f->sending->message.kind,
                                                          f->sending->message.data.clone() });
        }
    }
    if (terminal) {
        ws_dispose(active);
        return;
    }
    if (close.is_some()) {
        active.timer->start(
            int(rstd::min(state->options.close_timeout.as_millis() + u64(1), u64(2147483647))
                    .to_primitive()));
        auto bytes = close->as_slice();
        auto code =
            bytes.is_empty()
                ? QWebSocketProtocol::CloseCodeNormal
                : QWebSocketProtocol::CloseCode((unsigned(bytes[usize()].to_primitive()) << 8) |
                                                bytes[usize(1)].to_primitive());
        auto reason = bytes.is_empty()
                          ? QString()
                          : QString::fromUtf8(reinterpret_cast<const char*>(bytes.as_raw_ptr() + 2),
                                              qsizetype((bytes.len() - usize(2)).to_primitive()));
        socket->close(code, reason);
    }
    if (send.is_some()) {
        auto   bytes = ws_bytes(send->data);
        qint64 written {};
        if (send->kind == liwebsocketo::Kind::Text)
            written = socket->sendTextMessage(QString::fromUtf8(bytes));
        else if (send->kind == liwebsocketo::Kind::Binary)
            written = socket->sendBinaryMessage(bytes);
        else {
            socket->ping(bytes);
            written = bytes.size();
        }
        if (written != bytes.size())
            state->fail(WebSocketError::Transport, Some(ws_native_error(socket)));
    }
    if (active.writing && socket->bytesToWrite() == 0) {
        auto f         = state->fields.lock().unwrap();
        active.writing = false;
        if (f->sending.is_some()) (void)f->sending.take()->waiter.complete(Ok(empty {}));
    }
}
void WebSocketService::advance(QObject* owner, slice<usize> ids) {
    Vec<Arc<WsState>> queued;
    bool              closed;
    {
        auto f = state_->fields.lock().unwrap();
        queued = rstd::move(f->queued);
        closed = f->closed;
    }
    for (auto& state : queued) {
        if (closed) {
            state->fail(WebSocketError::Cancelled);
            state->complete();
        } else
            ws_start(state_, state.clone(), owner);
    }
    for (usize i {}; i < state_->active.len();) {
        auto& active = *state_->active[i];
        bool  ready  = closed;
        for (auto id : ids) ready = ready || id == active.state->dispatch.id();
        if (ready) {
            if (closed) active.state->fail(WebSocketError::Cancelled);
            ws_pump(active);
        }
        if (active.state->fields.lock().unwrap()->done)
            state_->active.remove(i);
        else
            ++i;
    }
}
} // namespace ncrequest::client::qt_network
