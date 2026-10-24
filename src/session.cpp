module ncrequest;
import :session;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest
{
namespace http = lifetchto::http;
struct SessionState {
    Option<Arc<lifetchto::Engine>>           engine;
    http::Client                             client;
    ResponseOptions                          response;
    Option<Arc<rstd::dyn<WebSocketBackend>>> websocket;
    SessionState(Option<Arc<lifetchto::Engine>> engine, http::Client client,
                 ResponseOptions response)
        : engine(rstd::move(engine)), client(rstd::move(client)), response(response) {}
};
struct PendingFields {
    Option<lifetchto::Transfer<http::ResponseMessage>> transfer;
    bool                                               reading {}, taken {}, abandoned {};
    explicit PendingFields(lifetchto::Transfer<http::ResponseMessage> transfer)
        : transfer(Some(rstd::move(transfer))) {}
};
struct PendingState {
    lifetchto::TransferControl       control;
    http::ExecutionObserver          observer;
    usize                            limit;
    rstd::sync::Mutex<PendingFields> fields;
    PendingState(lifetchto::Transfer<http::ResponseMessage> transfer,
                 http::ExecutionObserver observer, usize limit)
        : control(transfer.control()),
          observer(rstd::move(observer)),
          limit(limit),
          fields(PendingFields(rstd::move(transfer))) {}
};
struct PendingReadGuard {
    Arc<PendingState> state;
    ~PendingReadGuard() { state->fields.lock().unwrap()->reading = false; }
};
PendingResponse::PendingResponse(Arc<PendingState> state): state_(rstd::move(state)) {}
PendingResponse::PendingResponse(PendingResponse&&) noexcept = default;
PendingResponse::~PendingResponse() { abandon(); }
auto PendingResponse::operator=(PendingResponse&& other) noexcept -> PendingResponse& {
    if (this != &other) {
        abandon();
        state_ = rstd::move(other.state_);
    }
    return *this;
}
void PendingResponse::abandon() const {
    if (! state_) return;
    auto f = state_->fields.lock().unwrap();
    if (f->transfer.is_some()) {
        f->abandoned = true;
        state_->control.cancel();
    }
}
auto PendingResponse::receive(Arc<PendingState> state) -> coro<Result<Arc<Response>>> {
    Option<coro<lifetchto::Result<http::ResponseMessage>>> output;
    {
        auto f = state->fields.lock().unwrap();
        if (f->taken) co_return Err(Error::BodyTaken());
        if (f->abandoned) co_return Err(Error::Closed());
        if (f->reading) co_return Err(Error::Busy());
        f->reading = true;
        output     = Some(f->transfer->output());
    }
    PendingReadGuard guard { state.clone() };
    auto             received = co_await output.take().unwrap();
    if (received.is_err()) co_return Err(Error::Transport(rstd::move(received).unwrap_err()));
    auto f = state->fields.lock().unwrap();
    if (f->abandoned) co_return Err(Error::Closed());
    f->taken      = true;
    auto transfer = f->transfer.take().unwrap();
    co_return Ok(Response::make(rstd::move(transfer),
                                rstd::move(received).unwrap(),
                                state->observer.clone(),
                                state->limit));
}
auto PendingResponse::response() const -> coro<Result<Arc<Response>>> {
    return receive(state_.clone());
}
auto PendingResponse::completion() const -> lifetchto::TransferControl {
    return state_->control.clone();
}
void PendingResponse::cancel() const { state_->control.cancel(); }
auto PendingResponse::close() const -> coro<Arc<lifetchto::TransferReport>> {
    return state_->control.close();
}
Session::Session(ConstructionKey, Arc<SessionState> state): state_(rstd::move(state)) {}
auto Session::capabilities() const -> http::BackendCapabilities {
    return state_->client.capabilities();
}
auto Session::resources() const -> lifetchto::ResourceContext { return state_->client.resources(); }
auto Session::websocket_backend() const -> Option<Arc<rstd::dyn<WebSocketBackend>>> {
    return state_->websocket.clone();
}
auto Session::websocket_capabilities() const -> WebSocketCapabilities {
    if (state_->websocket.is_some()) return (*state_->websocket)->capabilities();
    bool supported = capabilities().duplex;
    return { supported, supported, supported, supported };
}
auto Session::prepare_connection(DuplexRequest input, DuplexOptions options) const
    -> rstd::Result<http::ConnectionPlan, lifetchto::Rejected<DuplexRequest>> {
    return state_->client.prepare_connection(rstd::move(input), options);
}
auto Session::duplex(DuplexRequest request, DuplexOptions options) const
    -> rstd::Result<PendingDuplex, lifetchto::Rejected<DuplexRequest>> {
    return state_->client.duplex(rstd::move(request), options);
}
auto Session::upgrade(UpgradeRequest request, UpgradeOptions options) const
    -> rstd::Result<PendingUpgrade, lifetchto::Rejected<UpgradeRequest>> {
    return state_->client.upgrade(rstd::move(request), options);
}
void stop_session(const Arc<SessionState>& state) {
    state->client.request_close();
    if (state->engine.is_some()) (*state->engine)->request_close();
}
Session::~Session() { stop_session(state_); }
auto Session::make(rstd::async::RuntimeHandle runtime, http::TransportOptions transport,
                   const SessionOptions& options) -> Result<Arc<Session>> {
    auto valid = options.http.validate();
    if (valid.is_err()) return Err(Error::Transport(rstd::move(valid).unwrap_err()));
    auto made = lifetchto::Engine::make(rstd::move(runtime));
    if (made.is_err()) return Err(Error::Transport(rstd::move(made).unwrap_err()));
    auto engine  = Arc<lifetchto::Engine>::make(rstd::move(made).unwrap());
    auto backend = http::NativeBackend::make(engine->resources(), rstd::move(transport));
    if (backend.is_err()) return Err(Error::Transport(rstd::move(backend).unwrap_err()));
    auto client = http::Client::make(
        engine->handle(), http::BackendBinding::Owned(backend->backend()), options.http.clone());
    if (client.is_err()) return Err(Error::Transport(rstd::move(client).unwrap_err()));
    auto state = Arc<SessionState>::make(
        Some(rstd::move(engine)), rstd::move(client).unwrap(), options.response);
    return Ok(Arc<Session>::make(ConstructionKey {}, rstd::move(state)));
}
auto Session::from_client(http::Client client, ResponseOptions options) -> Result<Arc<Session>> {
    auto state =
        Arc<SessionState>::make(None<Arc<lifetchto::Engine>>(), rstd::move(client), options);
    return Ok(Arc<Session>::make(ConstructionKey {}, rstd::move(state)));
}
auto Session::from_http_backend(lifetchto::EngineHandle engine, const http::BackendBinding& backend,
                                const SessionOptions& options) -> Result<Arc<Session>> {
    auto client = http::Client::make(rstd::move(engine), backend.clone(), options.http.clone());
    if (client.is_err()) return Err(Error::Transport(rstd::move(client).unwrap_err()));
    return from_client(rstd::move(client).unwrap(), options.response);
}
auto Session::from_backend(lifetchto::EngineHandle engine, const BackendBinding& backend,
                           const SessionOptions& options) -> Result<Arc<Session>> {
    auto websocket = backend.websocket();
    if (websocket.is_some() && ! *websocket)
        return Err(
            Error::Transport(lifetchto::Error::InvalidOptions("null WebSocket backend"_Str)));
    auto made = from_http_backend(rstd::move(engine), backend.http(), options);
    if (made.is_ok()) (*made)->state_->websocket = rstd::move(websocket);
    return made;
}
auto Session::send(Request request, Option<Arc<RequestControl>> control) const -> Submission {
    if (control.is_some()) {
        if (! *control)
            return Err(lifetchto::Rejected<Request> {
                lifetchto::Error::InvalidOptions(String::make("null request control"_str)),
                rstd::move(request) });
        auto claimed = (*control)->claim();
        if (claimed.is_err())
            return Err(lifetchto::Rejected<Request> { rstd::move(claimed).unwrap_err(),
                                                      rstd::move(request) });
    }
    auto options   = rstd::move(request.options_);
    auto execution = options.execution.clone();
    auto observer =
        execution.report.is_some() ? execution.report->clone() : http::ExecutionObserver {};
    execution.report = Some(observer.clone());
    auto limit       = options.collect_bytes.unwrap_or(state_->response.collect_bytes);
    auto accepted =
        state_->client.execute(rstd::move(request).into_message(), rstd::move(execution));
    if (accepted.is_err()) {
        auto rejected     = rstd::move(accepted).unwrap_err();
        auto original     = Request(rstd::move(rejected.input));
        original.options_ = rstd::move(options);
        return Err(
            lifetchto::Rejected<Request> { rstd::move(rejected.error), rstd::move(original) });
    }
    auto transfer = rstd::move(accepted).unwrap();
    if (control.is_some()) (*control)->bind(transfer.control());
    return Ok(PendingResponse(
        Arc<PendingState>::make(rstd::move(transfer), rstd::move(observer), limit)));
}
auto Session::get(Request request, Option<Arc<RequestControl>> control) const -> Submission {
    request.set_method(lihttpto::Method::parse(method::get).unwrap());
    return send(rstd::move(request), rstd::move(control));
}
auto Session::post(Request request, Option<Arc<RequestControl>> control) const -> Submission {
    request.set_method(lihttpto::Method::parse(method::post).unwrap());
    return send(rstd::move(request), rstd::move(control));
}
auto Session::post(Request request, Bytes body, Option<Arc<RequestControl>> control) const
    -> Submission {
    request.set_body(rstd::move(body));
    return post(rstd::move(request), rstd::move(control));
}
void Session::request_close() const { stop_session(state_); }
auto finish_session(Arc<SessionState> state) -> coro<lifetchto::CloseReport> {
    auto report = co_await state->client.close();
    if (state->engine.is_some()) report.merge(co_await (*state->engine)->close());
    co_return rstd::move(report);
}
auto Session::close() const -> coro<lifetchto::CloseReport> {
    request_close();
    return finish_session(state_.clone());
}
} // namespace ncrequest
