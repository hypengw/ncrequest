module;

#include <string>
#include <functional>
#include <span>
#include <curl/curl.h>

export module ncrequest:websocket;
export import ncrequest.type;

namespace ncrequest
{

class WebSocketClient {
public:
    using MessageCallback = std::function<void(std::span<const std::byte>)>;
    using ErrorCallback   = std::function<void(std::string)>;

    WebSocketClient();
    ~WebSocketClient();

    bool connect(const std::string& url);
    void disconnect();
    bool isConnected() const;

    bool send(std::string_view message);
    bool send(std::span<const std::byte> message);

    void set_on_message_callback(MessageCallback callback);
    void set_on_error_callback(ErrorCallback callback);

    auto on_message_callback() -> const MessageCallback&;

private:
    CURL*           m_curl;
    bool            m_connected;
    MessageCallback m_on_message;
    ErrorCallback   m_on_error;
};

} // namespace ncrequest