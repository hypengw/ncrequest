export module ncrequest:session;

export import :response;
export import :request_control;
#if defined(LITO_FEAT_QT)
export import :client.qt_network.backend;
#else
export import :client.curl.session;
#endif
export import :client.http_backend;
import :redirect;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;

namespace ncrequest
{

#if defined(LITO_FEAT_QT)
using SelectedSessionBackend = client::qt_network::SessionBackend;
#else
using SelectedSessionBackend = client::curl::SessionBackend;
#endif

static_assert(client::HttpSessionBackend<SelectedSessionBackend, SelectedResponseBackend>);

export class Session {
    struct ConstructionKey {};
    struct State {
        EffectiveOptions       options;
        SelectedSessionBackend backend;
        Atomic<bool>           closed { false };
        explicit State(EffectiveOptions value): options(rstd::move(value)) {}
#if defined(LITO_FEAT_QT)
        State(EffectiveOptions value, qt::QObject* parent)
            : options(rstd::move(value)), backend(parent) {}
        State(EffectiveOptions value, qt::QNetworkAccessManager* manager)
            : options(rstd::move(value)), backend(manager) {}
#endif
        void close() {
            if (! closed.exchange(true)) backend.close();
        }
    };
    Arc<State> state_;

public:
    Session(const Session&)                    = delete;
    auto operator=(const Session&) -> Session& = delete;

    Session(ConstructionKey, Arc<State> state): state_(rstd::move(state)) {}
    ~Session() { close(); }
    void close() { state_->close(); }

    static auto make(SessionOptions options = {}) -> Result<Arc<Session>> {
        auto effective = EffectiveOptions::resolve(options, RequestOptions {});
        if (effective.is_err()) return Err(rstd::move(effective).unwrap_err());
        auto initialized = SelectedSessionBackend::initialize();
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        return finish_make(Arc<State>::make(rstd::move(effective).unwrap()));
    }
#if defined(LITO_FEAT_QT)
    static auto from_qt_parent(qt::QObject* parent) -> Result<Arc<Session>> {
        auto initialized = SelectedSessionBackend::initialize(parent);
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto options = EffectiveOptions::resolve(SessionOptions {}, RequestOptions {}).unwrap();
        return finish_make(Arc<State>::make(rstd::move(options), parent));
    }
    static auto from_qt_manager(qt::QNetworkAccessManager* manager) -> Result<Arc<Session>> {
        auto initialized = SelectedSessionBackend::initialize(manager);
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto options = EffectiveOptions::resolve(SessionOptions {}, RequestOptions {}).unwrap();
        return finish_make(Arc<State>::make(rstd::move(options), manager));
    }
#endif

    auto get(Request req) -> coro<Result<Arc<Response>>> {
        req.set_method(lihttpto::Method::parse("GET"_str).unwrap());
        return send(rstd::move(req));
    }

    auto post(Request req) -> coro<Result<Arc<Response>>> {
        req.set_method(lihttpto::Method::parse("POST"_str).unwrap());
        return send(rstd::move(req));
    }

    auto post(Request req, Bytes body) -> coro<Result<Arc<Response>>> {
        req.set_body(rstd::move(body));
        return post(rstd::move(req));
    }

    auto send(Request req) -> coro<Result<Arc<Response>>> {
        return send_request(state_.clone(), rstd::move(req));
    }

    auto send(Request req, Arc<RequestControl> control) -> coro<Result<Arc<Response>>> {
        return send_request(state_.clone(), rstd::move(req), Some(rstd::move(control)));
    }

private:
    static auto finish_make(Arc<State> state) -> Result<Arc<Session>> {
        auto started = state->backend.start();
        if (started.is_err()) return Err(rstd::move(started).unwrap_err());
        return Ok(Arc<Session>::make(ConstructionKey {}, rstd::move(state)));
    }

    static auto send_request(Arc<State> state, Request req,
                             Option<Arc<RequestControl>> control = None())
        -> coro<Result<Arc<Response>>> {
        if (control.is_some()) {
            if (! *control) co_return Err(Error::InvalidState("request control is null"));
            if (! (*control)->claim())
                co_return Err(Error::InvalidState("request control was already used"));
            if ((*control)->is_canceled()) co_return Err(Error::Canceled());
#if defined(LITO_FEAT_QT)
            co_return Err(Error::Unsupported("Qt Network request control is not supported"));
#endif
        }
        if (state->closed.load()) co_return Err(Error::Canceled());
        auto effective = state->options.with_request(req.options());
        if (effective.is_err()) co_return Err(rstd::move(effective).unwrap_err());
        auto prepared = PreparedRequest::prepare(rstd::move(req), rstd::move(effective).unwrap());
        if (prepared.is_err()) co_return Err(rstd::move(prepared).unwrap_err());
        auto redirects = RedirectState(prepared->options().clone());
        for (;;) {
            if (state->closed.load()) co_return Err(Error::Canceled());
            if (control.is_some() && (*control)->is_canceled()) co_return Err(Error::Canceled());
            auto limits = prepared->options().limits();
#if defined(LITO_FEAT_QT)
            auto res = co_await state->backend.start_request(rstd::move(prepared).unwrap());
#else
            auto res = co_await state->backend.start_request(
                rstd::move(prepared).unwrap(),
                control.is_some() ? Some(control->clone()) : None<Arc<RequestControl>>());
#endif
            if (res.is_err()) co_return Err(rstd::move(res).unwrap_err());
            auto backend = rstd::move(res).unwrap();
            auto ready   = co_await backend.ready_head();
            if (ready.is_err()) co_return Err(rstd::move(ready).unwrap_err());
            auto head = backend.head();
            if (head.is_none()) co_return Err(Error::InvalidState("response head is unavailable"));
            auto next = redirects.next(backend.request(), **head);
            if (next.is_err()) co_return Err(rstd::move(next).unwrap_err());
            if (next->is_none()) co_return Response::make(rstd::move(backend), limits);
            backend.cancel();
            prepared = Ok(rstd::move(next).unwrap().unwrap());
        }
    }
};

} // namespace ncrequest
