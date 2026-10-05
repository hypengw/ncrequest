export module ncrequest:websocket;

#if defined(LITO_FEAT_QT)
export import :client.qt_network.websocket;
#else
export import :client.curl.websocket;
#endif
export import :client.websocket_backend;

namespace ncrequest
{

#if defined(LITO_FEAT_QT)
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
