export module ncrequest:websocket;
export import :websocket_connection;
export import :client.callback;
using namespace rstd::prelude;
export namespace ncrequest
{
struct WebSocketClientState;
class WebSocketClient {
    Arc<WebSocketClientState>            state_;
    Arc<lifetchto::ResourceLease<empty>> registration_;
    WebSocketClient(Arc<WebSocketClientState>, Arc<lifetchto::ResourceLease<empty>>);

public:
    WebSocketClient(WebSocketClient&&) noexcept;
    WebSocketClient(const WebSocketClient&) = delete;
    auto operator=(WebSocketClient&&) noexcept -> WebSocketClient&;
    ~WebSocketClient();
    static auto connect(Arc<Session>, DuplexRequest, WebSocketCallbacks, lihttpto::Headers = {},
                        Vec<String> = {}, WebSocketOptions = {})
        -> coro<rstd::Result<WebSocketClient, WebSocketConnectFailure>>;
    auto is_connected() const -> bool;
    auto send(liwebsocketo::Message) const -> rstd::Result<PendingSend, RejectedMessage>;
    auto request_close(Bytes = {}) const -> WebSocketResult<empty>;
    auto close(Bytes = {}) const -> coro<WebSocketResult<empty>>;
    auto failure() const -> Option<WebSocketError>;
    void disconnect() const;
    auto shutdown() const -> coro<lifetchto::Result<empty>>;
};
} // namespace ncrequest
