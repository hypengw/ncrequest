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

static_assert(client::HttpSessionBackend<SelectedSessionBackend, Response::Backend>);

export class Session : public SelectedSessionBackend {
public:
    using Backend = SelectedSessionBackend;

    using Backend::Backend;

    template<typename... Args>
    static auto make(Args&&... args) -> Arc<Session> {
        auto session = make_arc<Session>(rstd::forward<Args>(args)...);
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
        static_cast<Backend&>(*session).start();
#endif
        return session;
    }

    auto get(const Request& req) -> coro<Result<Arc<Response>>> {
        co_return co_await send(req, Operation::GetOperation, None<rstd::bytes::Bytes>());
    }

    auto post(const Request& req) -> coro<Result<Arc<Response>>> {
        co_return co_await post(req, rstd::bytes::Bytes::make());
    }

    auto post(const Request& req, rstd::bytes::Bytes body) -> coro<Result<Arc<Response>>> {
        co_return co_await send(req, Operation::PostOperation, Some(rstd::move(body)));
    }

private:
    auto send(const Request& req, Operation operation, rstd::Option<rstd::bytes::Bytes> body)
        -> coro<Result<Arc<Response>>> {
        auto res = co_await this->start_request(req, operation, rstd::move(body));
        if (res.is_err()) {
            co_return Result<Arc<Response>>(Err(rstd::move(res).unwrap_err()));
        }
        co_return Result<Arc<Response>>(Ok(make_arc<Response>(rstd::move(res).unwrap())));
    }
};

} // namespace ncrequest
