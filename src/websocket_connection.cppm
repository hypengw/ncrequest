export module ncrequest:websocket_connection;
export import :session;
export import liwebsocketo;
using namespace rstd::prelude;
export namespace ncrequest
{
struct WebSocketState;
struct WebSocketSendState;
struct WebSocketReceiveState;
class PendingSend {
    Arc<WebSocketState>     owner_;
    Arc<WebSocketSendState> state_;
    bool                    owned_ { true };
    friend class WebSocketConnection;
    PendingSend(Arc<WebSocketState>, Arc<WebSocketSendState>);

public:
    PendingSend(PendingSend&&) noexcept;
    PendingSend(const PendingSend&) = delete;
    auto operator=(PendingSend&&) noexcept -> PendingSend&;
    ~PendingSend();
    auto written() const -> coro<WebSocketResult<empty>>;
    void cancel() const;
};
class PendingReceive {
    Arc<WebSocketState>        owner_;
    Arc<WebSocketReceiveState> state_;
    bool                       owned_ { true };
    friend class WebSocketConnection;
    friend auto receive_from(const Arc<WebSocketState>&, bool) -> PendingReceive;
    PendingReceive(Arc<WebSocketState>, Arc<WebSocketReceiveState>);

public:
    PendingReceive(PendingReceive&&) noexcept;
    PendingReceive(const PendingReceive&) = delete;
    auto operator=(PendingReceive&&) noexcept -> PendingReceive&;
    ~PendingReceive();
    auto message() const -> coro<WebSocketResult<liwebsocketo::Message>>;
    void cancel() const;
    void close() const { cancel(); }
};
class WebSocketConnection {
    Arc<WebSocketState>        state_;
    lifetchto::Transfer<empty> transfer_;
    explicit WebSocketConnection(Arc<WebSocketState>, lifetchto::Transfer<empty>);
    friend struct WebSocketClientState;
    void fail(WebSocketError) const;

public:
    class MessageStream {
        friend class WebSocketConnection;
        Arc<WebSocketState>                                          state_;
        Option<PendingReceive>                                       pending_;
        Option<coro<WebSocketResult<liwebsocketo::Message>>::Future> waiting_;
        bool                                                         done_ {}, owned_ { true };
        explicit MessageStream(Arc<WebSocketState>);
        void release();

    public:
        using Item = WebSocketResult<liwebsocketo::Message>;
        MessageStream(MessageStream&&) noexcept;
        MessageStream(const MessageStream&) = delete;
        auto operator=(MessageStream&&) noexcept -> MessageStream&;
        ~MessageStream();
        auto poll_next(mut_ref<MessageStream>, rstd::task::Context&)
            -> rstd::task::Poll<Option<Item>>;
    };
    WebSocketConnection(WebSocketConnection&&) noexcept;
    WebSocketConnection(const WebSocketConnection&) = delete;
    auto operator=(WebSocketConnection&&) noexcept -> WebSocketConnection&;
    ~WebSocketConnection();
    static auto connect(Arc<Session>, DuplexRequest, lihttpto::Headers = {}, Vec<String> = {},
                        WebSocketOptions = {})
        -> coro<rstd::Result<WebSocketConnection, WebSocketConnectFailure>>;
    auto protocol() const -> Option<String>;
    auto send(liwebsocketo::Message) const -> rstd::Result<PendingSend, RejectedMessage>;
    auto receive() const -> PendingReceive;
    auto messages() const -> WebSocketResult<MessageStream>;
    auto request_close(Bytes payload = {}) const -> WebSocketResult<empty>;
    auto close(Bytes payload = {}) const -> coro<WebSocketResult<empty>>;
    void cancel() const;
    auto completion() const -> lifetchto::TransferControl;
    auto shutdown() const -> coro<lifetchto::Result<empty>>;
    auto failure() const -> Option<WebSocketError>;
};
} // namespace ncrequest
