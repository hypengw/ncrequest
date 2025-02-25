module;

#include <string>
#include <span>
#include <curl/curl.h>
#include <curl/websockets.h>

module ncrequest;
import :websocket;
import ncrequest.event;

namespace ncrequest
{

WebSocketClient::WebSocketClient(box<event::Context> ioc)
    : m_curl(curl_easy_init()), m_connected(false), m_context(std::move(ioc)) {
    m_context->set_error_callback([this](std::string_view error) {
        if (m_on_error) m_on_error(error);
    });
}

WebSocketClient::~WebSocketClient() {
    if (m_curl && m_context) {
        m_context->post([curl = m_curl] {
            curl_easy_cleanup(curl);
        });
    }
    m_curl = nullptr;
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
        size_t                      rlen { 0 };
        const struct curl_ws_frame* meta { nullptr };
        const CURLcode              result =
            curl_ws_recv(m_curl, m_read_buffer.data(), m_read_buffer.size(), &rlen, &meta);

        if (result == CURLE_OK && rlen > 0 && m_on_message) {
            m_on_message(std::span<const std::byte> { m_read_buffer.data(), rlen });
        }

        if (result == CURLE_AGAIN && m_connected) {
            do_read();
        }
    });
}

void WebSocketClient::disconnect() {
    m_context->post([this] {
        if (m_curl && m_connected) {
            m_context->cancel();
            m_context->close();
            curl_easy_reset(m_curl);
            m_connected = false;
        }
    });
}

bool WebSocketClient::is_connected() const { return m_connected; }

bool WebSocketClient::send(std::string_view message) {
    return send(std::span<const std::byte> { (const std::byte*)(message.data()), message.size() });
}

bool WebSocketClient::send(std::span<const std::byte> message) {
    if (! m_curl || ! m_connected) return false;

    size_t   sent { 0 };
    CURLcode result = curl_ws_send(m_curl, message.data(), message.size(), &sent, CURLWS_BINARY, 0);

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