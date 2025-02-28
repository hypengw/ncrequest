module;

#include <string>
#include <functional>
#include <span>
#include <optional>
#include <deque>
#include <memory_resource>
#include <curl/curl.h>

export module ncrequest:websocket;
export import ncrequest.type;
import ncrequest.event;
import rstd.rc;

namespace ncrequest
{

export class WebSocketClient {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 }; // 16KB
    using MessageCallback = std::function<void(std::span<const std::byte>, bool last)>;
    using ErrorCallback   = std::function<void(std::string_view)>;

    explicit WebSocketClient(
        box<event::Context> ioc, std::optional<u64> max_buffer_size = std::nullopt,
        std::pmr::memory_resource* mem_pool = std::pmr::get_default_resource());
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    bool connect(const std::string& url);
    void disconnect();
    bool is_connected() const;

    void send(std::string_view message);
    void send(std::span<const std::byte> message);

    void set_on_message_callback(MessageCallback callback);
    void set_on_error_callback(ErrorCallback callback);

    auto on_message_callback() -> const MessageCallback&;

private:
    void do_read();
    void do_write();
    void do_error(std::string_view);
    void reset_states();
    auto alloc(std::span<const std::byte>) -> std::span<const std::byte>;
    auto dealloc(std::span<const std::byte>);

    CURL*           m_curl;
    bool            m_connected;
    MessageCallback m_on_message;
    ErrorCallback   m_on_error;

    std::pmr::polymorphic_allocator<std::byte>       m_alloc;
    std::pmr::vector<std::byte>                      m_read_buffer;
    u64                                              m_read_len;
    std::pmr::deque<rstd::rc::Rc<const std::byte[]>> m_msgs;
    u64                                              m_sent_len;

    box<event::Context> m_context;
};

} // namespace ncrequest