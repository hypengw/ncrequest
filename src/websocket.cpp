module;

#include <string>
#include <span>
#include <curl/curl.h>
#include <curl/websockets.h>

module ncrequest;
import :websocket;

namespace
{
size_t recv_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* client = static_cast<ncrequest::WebSocketClient*>(userdata);
    if (client && client->on_message_callback()) {
        const auto data_size = size * nitems;
        client->on_message_callback()(
            std::span<const std::byte> { (const std::byte*)(buffer), data_size });
    }
    return size * nitems;
}
} // namespace

namespace ncrequest
{

WebSocketClient::WebSocketClient(): m_curl(curl_easy_init()), m_connected(false) {
    if (m_curl) {
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, recv_callback);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
    }
}

WebSocketClient::~WebSocketClient() {
    disconnect();
    if (m_curl) {
        curl_easy_cleanup(m_curl);
    }
}

bool WebSocketClient::connect(const std::string& url) {
    if (! m_curl || m_connected) return false;

    curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_curl, CURLOPT_CONNECT_ONLY, 2L); // WebSocket mode

    CURLcode result = curl_easy_perform(m_curl);
    if (result != CURLE_OK) {
        if (m_on_error) {
            m_on_error(curl_easy_strerror(result));
        }
        return false;
    }

    m_connected = true;
    return true;
}

void WebSocketClient::disconnect() {
    if (m_curl && m_connected) {
        curl_ws_send(m_curl, nullptr, 0, nullptr, CURLWS_CLOSE, 0);
        m_connected = false;
    }
}

bool WebSocketClient::isConnected() const { return m_connected; }

bool WebSocketClient::send(std::string_view message) {
    return send(std::span<const std::byte> { (const std::byte*)(message.data()), message.size() });
}

bool WebSocketClient::send(std::span<const std::byte> message) {
    if (! m_curl || ! m_connected) return false;

    size_t   sent { 0 };
    CURLcode result =
        curl_ws_send(m_curl, (const void*)message.data(), message.size(), &sent, CURLWS_BINARY, 0);

    if (result != CURLE_OK) {
        if (m_on_error) {
            m_on_error(curl_easy_strerror(result));
        }
        return false;
    }

    return sent == message.size();
}

auto WebSocketClient::on_message_callback() -> const MessageCallback& { return m_on_message; }

} // namespace ncrequest