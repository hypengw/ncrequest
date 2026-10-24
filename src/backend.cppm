export module ncrequest:backend;
export import :websocket_backend;
using namespace rstd::prelude;
export namespace ncrequest
{
class BackendBinding {
    lifetchto::http::BackendBinding          http_;
    Option<Arc<rstd::dyn<WebSocketBackend>>> websocket_;
    BackendBinding(lifetchto::http::BackendBinding          http,
                   Option<Arc<rstd::dyn<WebSocketBackend>>> websocket)
        : http_(rstd::move(http)), websocket_(rstd::move(websocket)) {}

public:
    static auto Owned(Arc<rstd::dyn<lifetchto::http::ExchangeBackend>> http,
                      Option<Arc<rstd::dyn<WebSocketBackend>>> websocket = {}) -> BackendBinding {
        return BackendBinding(lifetchto::http::BackendBinding::Owned(rstd::move(http)),
                              rstd::move(websocket));
    }
    static auto Shared(Arc<rstd::dyn<lifetchto::http::ExchangeBackend>> http,
                       Option<Arc<rstd::dyn<WebSocketBackend>>> websocket = {}) -> BackendBinding {
        return BackendBinding(lifetchto::http::BackendBinding::Shared(rstd::move(http)),
                              rstd::move(websocket));
    }
    auto http() const -> lifetchto::http::BackendBinding { return http_.clone(); }
    auto websocket() const -> Option<Arc<rstd::dyn<WebSocketBackend>>> {
        return websocket_.clone();
    }
};
} // namespace ncrequest
