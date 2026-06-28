module;
#include <functional>
#include <future>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

export module ncrequest:client_curl_websocket;
export import ncrequest.type;
export import ncrequest.curl;
import rstd;

namespace ncrequest::client::curl
{

export class WebSocketBackend {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 }; // 16KB
    using ConnectedCallback = std::function<void()>;
    using MessageCallback   = std::function<void(std::span<const rstd::byte>, bool last)>;
    using ErrorCallback     = std::function<void(rstd::ref<rstd::str>)>;

    explicit WebSocketBackend(
        rstd::Option<u64>          max_buffer_size = None(),
        std::pmr::memory_resource* mem_pool        = std::pmr::get_default_resource());
    ~WebSocketBackend();
    WebSocketBackend(const WebSocketBackend&)            = delete;
    WebSocketBackend& operator=(const WebSocketBackend&) = delete;

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
    class Impl;

    Box<Impl> m_impl;
};

} // namespace ncrequest::client::curl
