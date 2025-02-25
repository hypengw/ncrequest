module;

#include <string>
#include <functional>
#include <span>
#include <curl/curl.h>

export module ncrequest:websocket;
export import ncrequest.type;
import ncrequest.event;

namespace ncrequest
{

class WebSocketClient {
public:
    using MessageCallback = std::function<void(std::span<const std::byte>)>;
    using ErrorCallback   = std::function<void(std::string_view)>;

    explicit WebSocketClient(box<event::Context> ioc);
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    bool connect(const std::string& url);
    void disconnect();
    bool is_connected() const;

    bool send(std::string_view message);
    bool send(std::span<const std::byte> message);

    void set_on_message_callback(MessageCallback callback);
    void set_on_error_callback(ErrorCallback callback);

    auto on_message_callback() -> const MessageCallback&;

private:
    void start_socket_monitor();
    void do_read();

    CURL*           m_curl;
    bool            m_connected;
    MessageCallback m_on_message;
    ErrorCallback   m_on_error;

    box<event::Context>              m_context;
    std::array<std::byte, 64 * 1024> m_read_buffer;
};

} // namespace ncrequest