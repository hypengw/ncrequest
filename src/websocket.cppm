export module ncrequest:websocket;

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
export import :client_qt_network_websocket;
#else
export import :client_curl_websocket;
#endif
export import :client_websocket_backend;

namespace ncrequest
{

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
using SelectedWebSocketBackend = client::qt_network::WebSocketBackend;
#else
using SelectedWebSocketBackend = client::curl::WebSocketBackend;
#endif

static_assert(client::WebSocketBackend<SelectedWebSocketBackend>);

export class WebSocketClient : public SelectedWebSocketBackend {
public:
    using Backend              = SelectedWebSocketBackend;
    using ConnectedCallback    = Backend::ConnectedCallback;
    using DisconnectedCallback = Backend::DisconnectedCallback;
    using MessageCallback      = Backend::MessageCallback;
    using ErrorCallback        = Backend::ErrorCallback;

    using Backend::Backend;
};

} // namespace ncrequest
