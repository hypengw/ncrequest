export module ncrequest:websocket;

#if defined(NCREQUEST_WEBSOCKET_BACKEND_QT_WEBSOCKETS)
export import :client_qt_websockets;
#else
export import :client_curl_websocket;
#endif
export import :client_websocket_backend;

namespace ncrequest
{

#if defined(NCREQUEST_WEBSOCKET_BACKEND_QT_WEBSOCKETS)
using SelectedWebSocketBackend = client::qt_websockets::WebSocketBackend;
#else
using SelectedWebSocketBackend = client::curl::WebSocketBackend;
#endif

static_assert(client::WebSocketBackend<SelectedWebSocketBackend>);

export class WebSocketClient : public SelectedWebSocketBackend {
public:
    using Backend           = SelectedWebSocketBackend;
    using ConnectedCallback = Backend::ConnectedCallback;
    using MessageCallback   = Backend::MessageCallback;
    using ErrorCallback     = Backend::ErrorCallback;

    using Backend::Backend;
};

} // namespace ncrequest
