export module ncrequest:session;

export import :response;
#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
export import :client_qt_network;
#else
export import :client_curl_session;
#endif
export import :client_http_backend;

namespace ncrequest
{

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
using SelectedSessionBackend = client::qt_network::SessionBackend;
#else
using SelectedSessionBackend = client::curl::SessionBackend;
#endif

static_assert(client::HttpSessionBackend<SelectedSessionBackend, SelectedResponseBackend>);

export class Session : public NoCopy {
    struct ConstructionKey {};
    struct State {
        SelectedSessionBackend           backend;
        rstd::sync::atomic::Atomic<bool> closed { false };
        State() = default;
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
    Session(): state_(Arc<State>::make()) { start_backend(state_->backend); }
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

    auto get(Request req) -> coro<Result<Arc<Response>>> {
        return send(state_.clone(), rstd::move(req), Operation::Get(), None<rstd::bytes::Bytes>());
    }

    auto post(Request req) -> coro<Result<Arc<Response>>> {
        return post(rstd::move(req), rstd::bytes::Bytes::make());
    }

    auto post(Request req, rstd::bytes::Bytes body) -> coro<Result<Arc<Response>>> {
        return send(state_.clone(), rstd::move(req), Operation::Post(), Some(rstd::move(body)));
    }

private:
    template<typename T>
    static void start_backend(T& backend) {
        if constexpr (requires(T& value) { value.start(); }) {
            backend.start();
        }
    }

    static auto send(Arc<State> state, Request req, Operation operation,
                     rstd::Option<rstd::bytes::Bytes> body) -> coro<Result<Arc<Response>>> {
        if (state->closed.load()) co_return Err(Error::Canceled());
        auto res = co_await state->backend.start_request(req, operation, rstd::move(body));
        if (res.is_err()) {
            co_return Result<Arc<Response>>(Err(rstd::move(res).unwrap_err()));
        }
        auto backend = rstd::move(res).unwrap();
        auto ready   = co_await backend.ready_head();
        if (ready.is_err()) co_return Err(rstd::move(ready).unwrap_err());
        co_return Response::make(rstd::move(backend));
    }
};

} // namespace ncrequest
