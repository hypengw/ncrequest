export module ncrequest:websocket;
export import ncrequest.type;
export import ncrequest.curl;
import ncrequest.event;
import rstd;

namespace ncrequest
{

export class WebSocketClient {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 }; // 16KB
    using ConnectedCallback = std::function<void()>;
    using MessageCallback =
        std::function<void(std::span<const rstd::byte>, bool last)>;
    using ErrorCallback = std::function<void(rstd::ref<rstd::str>)>;

    explicit WebSocketClient(
        Box<event::Context> ioc, rstd::Option<u64> max_buffer_size = None(),
        std::pmr::memory_resource* mem_pool = std::pmr::get_default_resource());
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    auto connect(const std::string& url) -> std::future<bool>;
    void disconnect();
    bool is_connected() const;

    void send(std::string_view message);
    void send(std::span<const rstd::byte> message);

    void set_on_connected_callback(ConnectedCallback callback);
    void set_on_message_callback(MessageCallback callback);
    void set_on_error_callback(ErrorCallback callback);

    auto on_message_callback() -> const MessageCallback&;

private:
    void do_read();
    void do_write();
    void do_error(curl::CURLcode);
    void do_disconnect(bool send);
    void reset_states();
    auto alloc(std::span<const rstd::byte>) -> std::span<const rstd::byte>;
    auto dealloc(std::span<const rstd::byte>);

    curl::CURL*       m_curl;
    bool              m_connected;
    ConnectedCallback m_on_connected;
    MessageCallback   m_on_message;
    ErrorCallback     m_on_error;

    std::pmr::polymorphic_allocator<rstd::byte>       m_alloc;
    std::pmr::vector<rstd::byte>                      m_read_buffer;
    u64                                                        m_read_len;
    std::pmr::deque<rstd::rc::Rc<const rstd::byte[]>> m_msgs;
    u64                                                        m_sent_len;

    Box<event::Context> m_context;
};

} // namespace ncrequest