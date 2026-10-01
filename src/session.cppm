export module ncrequest:session;

export import :response;
#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
export import :client_qt_network;
#else
export import :client_curl_session;
#endif
export import :client_http_backend;
import :redirect;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;

namespace ncrequest
{

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
using SelectedSessionBackend = client::qt_network::SessionBackend;
#else
using SelectedSessionBackend = client::curl::SessionBackend;
#endif

static_assert(client::HttpSessionBackend<SelectedSessionBackend, SelectedResponseBackend>);

export class Session {
    struct ConstructionKey {};
    struct State {
        SessionOptions         options;
        SelectedSessionBackend backend;
        Atomic<bool>           closed { false };
        State() = default;
        explicit State(SessionOptions value): options(rstd::move(value)) {}
#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
        explicit State(qt::QObject* parent): backend(parent) {}
        explicit State(qt::QNetworkAccessManager* manager): backend(manager) {}
#endif
        void close() {
            if (! closed.exchange(true)) backend.close();
        }
    };
    Arc<State> state_;

public:
    Session(const Session&)                    = delete;
    auto operator=(const Session&) -> Session& = delete;

    Session(): state_(Arc<State>::make()) { start_backend(state_->backend); }
    explicit Session(SessionOptions options): state_(Arc<State>::make(rstd::move(options))) {
        start_backend(state_->backend);
    }
    ~Session() { close(); }
#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
    Session(ConstructionKey, qt::QObject* parent): state_(Arc<State>::make(parent)) {}
    Session(ConstructionKey, qt::QNetworkAccessManager* manager)
        : state_(Arc<State>::make(manager)) {}
    static auto from_qt_parent(qt::QObject* parent) -> Arc<Session> {
        return Arc<Session>::make(ConstructionKey {}, parent);
    }
    static auto from_qt_manager(qt::QNetworkAccessManager* manager) -> Arc<Session> {
        return Arc<Session>::make(ConstructionKey {}, manager);
    }
#endif
    void close() { state_->close(); }

    static auto make() -> Arc<Session> { return Arc<Session>::make(); }
    static auto make(SessionOptions options) -> Arc<Session> {
        return Arc<Session>::make(rstd::move(options));
    }

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

private:
    template<typename T>
    static void start_backend(T& backend) {
        if constexpr (requires(T& value) { value.start(); }) {
            backend.start();
        }
    }

    static auto send_request(Arc<State> state, Request req) -> coro<Result<Arc<Response>>> {
        if (state->closed.load()) co_return Err(Error::Canceled());
        auto prepared = PreparedRequest::prepare(rstd::move(req), state->options);
        if (prepared.is_err()) co_return Err(rstd::move(prepared).unwrap_err());
        auto redirects = RedirectState(prepared->options().clone());
        for (;;) {
            if (state->closed.load()) co_return Err(Error::Canceled());
            auto res = co_await state->backend.start_request(rstd::move(prepared).unwrap());
            if (res.is_err()) co_return Err(rstd::move(res).unwrap_err());
            auto backend = rstd::move(res).unwrap();
            auto ready   = co_await backend.ready_head();
            if (ready.is_err()) co_return Err(rstd::move(ready).unwrap_err());
            auto head = backend.head();
            if (head.is_none()) co_return Err(Error::InvalidState("response head is unavailable"));
            auto next = redirects.next(backend.request(), **head);
            if (next.is_err()) co_return Err(rstd::move(next).unwrap_err());
            if (next->is_none()) co_return Response::make(rstd::move(backend));
            backend.cancel();
            prepared = Ok(rstd::move(next).unwrap().unwrap());
        }
    }
};

} // namespace ncrequest
