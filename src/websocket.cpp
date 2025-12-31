module ncrequest;
import :websocket;
import ncrequest.event;
import rstd.rc;

using namespace curl;

namespace ncrequest
{

WebSocketClient::WebSocketClient(Box<event::Context> ioc, rstd::Option<u64> max_buffer_size,
                                 rstd::cppstd::pmr::memory_resource* mem_pool)
    : m_curl(curl_easy_init()),
      m_connected(false),
      m_alloc(mem_pool),
      m_read_buffer(max_buffer_size.unwrap_or(MaxBufferSize), m_alloc),
      m_read_len(0),
      m_msgs(m_alloc),
      m_sent_len(0),
      m_context(rstd::move(ioc)) {
    m_context->set_error_callback([this](rstd::cppstd::string_view error) {
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

auto WebSocketClient::connect(const rstd::cppstd::string& url) -> rstd::cppstd::future<bool> {
    auto promise = make_arc<rstd::cppstd::promise<bool>>();
    auto future  = promise->get_future();

    m_context->post([this, url, promise] {
        if (! m_curl || m_connected) {
            promise->set_value(false);
            return;
        }

        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_CONNECT_ONLY, 2L); // WebSocket mode
        // curl_easy_setopt(m_curl, CURLOPT_VERBOSE, 1L);

        CURLcode result = curl_easy_perform(m_curl);
        if (result != CURLE_OK) {
            if (m_on_error) {
                m_on_error(curl_easy_strerror(result));
            }
            promise->set_value(false);
            return;
        }

        m_connected = true;
        m_read_len  = 0;
        m_sent_len  = 0;

        curl_socket_t sockfd;
        curl_easy_getinfo(m_curl, CURLINFO_ACTIVESOCKET, &sockfd);

        bool success = m_context->assign(sockfd);
        if (success) {
            m_context->wait(event::WaitType::Read, [this] {
                do_read();
            });
            m_context->wait(event::WaitType::Write, [this] {
                do_write();
            });
        }
        promise->set_value(success);
        if (m_on_connected) {
            m_on_connected();
        }
    });
    promise.reset();
    return future;
}

void WebSocketClient::do_read() {
    usize                       rlen { 0 };
    const struct curl_ws_frame* meta { nullptr };
    CURLcode                    result = CURLE_OK;

    for (;;) {
        {
            auto data = m_read_buffer.data() + m_read_len;
            auto size = m_read_buffer.size() - m_read_len;
            result    = curl_ws_recv(m_curl, data, size, &rlen, &meta);
        }

        m_read_len += rlen;
        if (result == CURLE_AGAIN) {
            break;
        }
        if (result != CURLE_OK) {
            do_error(result);
            return;
        }
        {
            bool last = ! (meta->flags & CURLWS_CONT) && meta->bytesleft == 0;
            if (last || m_read_buffer.size() == m_read_len || rlen == 0) {
                if (m_on_message) {
                    m_on_message(
                        rstd::cppstd::span<const rstd::byte> { m_read_buffer.data(), m_read_len },
                        last);
                }
                m_read_len = 0;
            }
        }

        if (! m_connected) {
            return;
        }
    }

    m_context->wait(event::WaitType::Read, [this] {
        do_read();
    });
}

void WebSocketClient::do_write() {
    if (! m_curl || ! m_connected) return;

    if (m_msgs.empty()) return;

    auto msg = m_msgs.front();

    usize    sent { 0 };
    CURLcode result = CURLE_OK;

    for (;;) {
        {
            auto data = msg.get() + m_sent_len;
            auto size = msg.size() - m_sent_len;
            result    = curl_ws_send(m_curl, data, size, &sent, 0, CURLWS_BINARY);
        }

        m_sent_len += sent;

        if (result == CURLE_AGAIN) {
            break;
        }
        if (result != CURLE_OK) {
            do_error(result);
            return;
        }

        // printf("send(%d): %.*s\n", (int)m_sent_len, (int)msg.size(), (const char*)msg.get());

        m_sent_len = 0;
        m_msgs.pop_front();

        if (m_msgs.empty() || ! m_connected) return;
    }

    m_context->wait(event::WaitType::Write, [this] {
        do_write();
    });
}

void WebSocketClient::do_error(CURLcode code) {
    do_disconnect(false);
    if (m_on_error) {
        m_on_error(rstd::format("{}({})", curl_easy_strerror(code), (int)code));
    }
}

void WebSocketClient::reset_states() {
    m_connected = false;
    m_read_len  = 0;
    m_sent_len  = 0;
}

void WebSocketClient::disconnect() {
    m_context->post([this] {
        this->do_disconnect(true);
    });
}

void WebSocketClient::do_disconnect(bool send) {
    if (m_curl && m_connected) {
        m_context->cancel();
        if (send) {
            (void)curl_ws_send(m_curl, "", 0, nullptr, 0, CURLWS_CLOSE);
        }
        m_context->reset();
        curl_easy_cleanup(m_curl);
        m_curl = curl_easy_init();
        reset_states();
    }
}

bool WebSocketClient::is_connected() const { return m_connected; }

void WebSocketClient::send(rstd::cppstd::string_view message) {
    send(std::span<const rstd::byte> { (const rstd::byte*)(message.data()), message.size() });
}

void WebSocketClient::send(rstd::cppstd::span<const rstd::byte> in) {
    auto msg = rstd::rc::allocate_make_rc<rstd::byte[]>(m_alloc, in.size(), rstd::byte {});
    rstd::copy_n(in.begin(), in.size(), msg.get());

    m_context->post([this, msg = rstd::move(msg)] {
        m_msgs.emplace_back(msg);
        do_write();
    });
}

auto WebSocketClient::on_message_callback() -> const MessageCallback& { return m_on_message; }
void WebSocketClient::set_on_connected_callback(ConnectedCallback cb) { m_on_connected = cb; }
void WebSocketClient::set_on_message_callback(MessageCallback cb) { m_on_message = cb; }
void WebSocketClient::set_on_error_callback(ErrorCallback cb) { m_on_error = cb; }

} // namespace ncrequest