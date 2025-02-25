module;

#include <string>
#include <span>
#include <curl/curl.h>
#include <curl/websockets.h>
#include <asio.hpp>

module ncrequest;
import :websocket;
import ncrequest.event;

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

WebSocketClient::WebSocketClient(asio::io_context& ioc)
    : m_curl(curl_easy_init()), m_context(event::create(ioc)) {
    m_context->set_error_callback([this](std::string_view error) {
        if (m_on_error) m_on_error(error);
    });
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
    start_socket_monitor();
    return true;
}

void WebSocketClient::start_socket_monitor() {
    curl_socket_t sockfd;
    curl_easy_getinfo(m_curl, CURLINFO_ACTIVESOCKET, &sockfd);

    if (m_context->assign(sockfd)) {
        do_read();
    }
}

void WebSocketClient::do_read() {
    m_context->wait(event::WaitType::Read, [this]() {
        size_t         rlen;
        const CURLcode result =
            curl_ws_recv(m_curl, m_read_buffer.data(), m_read_buffer.size(), &rlen, nullptr);

        if (result == CURLE_OK && rlen > 0 && m_on_message) {
            m_on_message(std::span<const std::byte> { m_read_buffer.data(), rlen });
        }

        if (m_connected) {
            do_read();
        }
    });
}

void WebSocketClient::disconnect() {
    if (m_curl && m_connected) {
        m_context->cancel();
        m_context->close();
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
        curl_ws_send(m_curl, message.data(), message.size(), &sent, CURLWS_BINARY, 0);

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