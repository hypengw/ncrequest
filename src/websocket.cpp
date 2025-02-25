module;

#include <string>
#include <span>
#include <format>
#include <future>
#include <curl/curl.h>
#include <curl/websockets.h>

module ncrequest;
import :websocket;
import ncrequest.event;

namespace ncrequest
{

WebSocketClient::WebSocketClient(box<event::Context> ioc, std::optional<u64> max_buffer_size)
    : m_curl(curl_easy_init()),
      m_connected(false),
      m_context(std::move(ioc)),
      m_read_buffer(max_buffer_size.value_or(MaxBufferSize)),
      m_read_len(0) {
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
    m_context->post([this, url] {
        if (! m_curl || m_connected) return;

        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_CONNECT_ONLY, 2L); // WebSocket mode
        // curl_easy_setopt(m_curl, CURLOPT_VERBOSE, 1L);

        CURLcode result = curl_easy_perform(m_curl);
        if (result != CURLE_OK) {
            if (m_on_error) {
                m_on_error(curl_easy_strerror(result));
            }
            return;
        }

        m_connected = true;
        m_read_len  = 0;
        curl_socket_t sockfd;
        curl_easy_getinfo(m_curl, CURLINFO_ACTIVESOCKET, &sockfd);

        if (m_context->assign(sockfd)) {
            m_context->wait(event::WaitType::Read, [this] {
                do_read();
            });
            m_context->wait(event::WaitType::Write, [this] {
                do_write();
            });
        }
    });
    return true;
}

void WebSocketClient::do_read() {
    size_t                      rlen { 0 };
    const struct curl_ws_frame* meta { nullptr };
    const CURLcode              result = curl_ws_recv(
        m_curl, m_read_buffer.data() + m_read_len, m_read_buffer.size() - m_read_len, &rlen, &meta);

    m_read_len += rlen;

    do {
        if (result == CURLE_OK && rlen > 0 && m_on_message) {
            bool last = ! (meta->flags & CURLWS_CONT) && meta->bytesleft == 0;
            m_on_message(std::span<const std::byte> { m_read_buffer.data(), m_read_len }, last);
            if (last) {
                m_read_len = 0;
            }
        }

        if (result == CURLE_GOT_NOTHING || ! m_connected) {
            return;
        }
    } while (0);

    m_context->wait(event::WaitType::Read, [this] {
        do_read();
    });
}

void WebSocketClient::do_write() {}

void WebSocketClient::disconnect() {
    m_context->post([this] {
        if (m_curl && m_connected) {
            m_context->cancel();
            m_context->close();
            (void)curl_ws_send(m_curl, "", 0, nullptr, 0, CURLWS_CLOSE);
            curl_easy_reset(m_curl);
            m_connected = false;
        }
    });
}

bool WebSocketClient::is_connected() const { return m_connected; }

void WebSocketClient::send(std::string_view message) {
    send(std::span<const std::byte> { (const std::byte*)(message.data()), message.size() });
}

void WebSocketClient::send(std::span<const std::byte> message) {
    m_context->post([this, message] {
        if (! m_curl || ! m_connected) return false;

        size_t   sent { 0 };
        CURLcode result =
            curl_ws_send(m_curl, message.data(), message.size(), &sent, 0, CURLWS_BINARY);

        if (result != CURLE_OK) {
            if (m_on_error) {
                m_on_error(std::format("{}({})", curl_easy_strerror(result), (int)result));
            }
            return false;
        }

        return sent == message.size();
    });
}

auto WebSocketClient::on_message_callback() -> const MessageCallback& { return m_on_message; }
void WebSocketClient::set_on_message_callback(MessageCallback cb) { m_on_message = cb; }
void WebSocketClient::set_on_error_callback(ErrorCallback cb) { m_on_error = cb; }

} // namespace ncrequest