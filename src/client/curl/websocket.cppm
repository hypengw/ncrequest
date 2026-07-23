module;
#include <memory_resource>

export module ncrequest:client_curl_websocket;
export import ncrequest.type;
export import ncrequest.curl;
export import :client_callback;
import rstd;

namespace ncrequest::client::curl
{

export class WebSocketBackend {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 }; // 16KB
    using ConnectedCallback    = client::Callback<void()>;
    using DisconnectedCallback = client::Callback<void()>;
    using MessageCallback      = client::Callback<void(slice<u8>, bool last)>;
    using ErrorCallback        = client::Callback<void(rstd::ref<rstd::str>)>;

    explicit WebSocketBackend(
        rstd::Option<u64>          max_buffer_size = None(),
        std::pmr::memory_resource* mem_pool        = std::pmr::get_default_resource());
    ~WebSocketBackend();
    WebSocketBackend(const WebSocketBackend&)            = delete;
    WebSocketBackend& operator=(const WebSocketBackend&) = delete;

    auto connect(ref<str> url) -> rstd::async::Completion<bool>;
    void disconnect();
    bool is_connected() const;

    void send(ref<str> message);
    void send(slice<u8> message);

    void set_on_connected_callback(ConnectedCallback callback);
    void set_on_disconnected_callback(DisconnectedCallback callback);
    void set_on_message_callback(MessageCallback callback);
    void set_on_error_callback(ErrorCallback callback);

private:
    class Impl;

    Box<Impl> m_impl;
};

} // namespace ncrequest::client::curl
