export module ncrequest:websocket;
export import ncrequest.type;
export import ncrequest.curl;
import ncrequest.event;
import rstd.rc;

namespace ncrequest
{

export class WebSocketClient {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 }; // 16KB
    using ConnectedCallback = rstd::cppstd::function<void()>;
    using MessageCallback =
        rstd::cppstd::function<void(rstd::cppstd::span<const rstd::byte>, bool last)>;
    using ErrorCallback = rstd::cppstd::function<void(rstd::ref<rstd::str>)>;

    explicit WebSocketClient(
        Box<event::Context> ioc, rstd::Option<u64> max_buffer_size = None(),
        rstd::cppstd::pmr::memory_resource* mem_pool = rstd::cppstd::pmr::get_default_resource());
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    auto connect(const rstd::cppstd::string& url) -> rstd::cppstd::future<bool>;
    void disconnect();
    bool is_connected() const;

    void send(rstd::cppstd::string_view message);
    void send(rstd::cppstd::span<const rstd::byte> message);

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
    auto alloc(rstd::cppstd::span<const rstd::byte>) -> rstd::cppstd::span<const rstd::byte>;
    auto dealloc(rstd::cppstd::span<const rstd::byte>);

    curl::CURL*       m_curl;
    bool              m_connected;
    ConnectedCallback m_on_connected;
    MessageCallback   m_on_message;
    ErrorCallback     m_on_error;

    rstd::cppstd::pmr::polymorphic_allocator<rstd::byte>       m_alloc;
    rstd::cppstd::pmr::vector<rstd::byte>                      m_read_buffer;
    u64                                                        m_read_len;
    rstd::cppstd::pmr::deque<rstd::rc::Rc<const rstd::byte[]>> m_msgs;
    u64                                                        m_sent_len;

    Box<event::Context> m_context;
};

} // namespace ncrequest