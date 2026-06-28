module;
#include <algorithm>
#include <atomic>
#include <deque>
#include <future>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

module ncrequest;
import :client_curl_websocket;
import rstd;

using namespace ::curl;

namespace ncrequest::client::curl
{

class WebSocketBackend::Impl {
    struct ConnectCommand {
        std::string             url;
        Arc<std::promise<bool>> promise;
    };

    struct SendCommand {
        rstd::rc::Rc<const rstd::byte[]> message;
    };

    struct DisconnectCommand {
        bool send_close { false };
    };

    struct StopCommand {};

    using Command = std::variant<ConnectCommand, SendCommand, DisconnectCommand, StopCommand>;

    struct QueueClosed {};
    struct Readable {};
    struct Writable {};
    struct IoError {
        rstd::io::error::Error error;
    };

    using LoopEvent = std::variant<QueueClosed, Command, Readable, Writable, IoError>;

    class CommandQueue {
    public:
        using Output = rstd::Option<Command>;

        auto push(Command command) -> bool {
            auto waker = rstd::Option<rstd::task::Waker> {};
            {
                auto lock = std::lock_guard { m_mutex };
                if (m_closed) return false;

                m_commands.emplace_back(rstd::move(command));
                waker = m_waker.take();
            }

            if (waker.is_some()) {
                rstd::move(*waker).wake();
            }
            return true;
        }

        void close() {
            auto waker = rstd::Option<rstd::task::Waker> {};
            {
                auto lock = std::lock_guard { m_mutex };
                if (m_closed) return;

                m_closed = true;
                waker    = m_waker.take();
            }

            if (waker.is_some()) {
                rstd::move(*waker).wake();
            }
        }

        void clear_waker() {
            auto lock = std::lock_guard { m_mutex };
            m_waker   = rstd::None();
        }

        auto poll_receive(rstd::task::Context& cx) -> rstd::task::Poll<Output> {
            auto lock = std::lock_guard { m_mutex };
            if (! m_commands.empty()) {
                auto command = rstd::move(m_commands.front());
                m_commands.pop_front();
                return rstd::task::Poll<Output>::Ready(rstd::Some(rstd::move(command)));
            }

            if (m_closed) {
                return rstd::task::Poll<Output>::Ready(rstd::None<Command>());
            }

            m_waker = rstd::Some(cx.waker().clone());
            return rstd::task::Poll<Output>::Pending();
        }

    private:
        std::mutex                      m_mutex;
        std::deque<Command>             m_commands;
        rstd::Option<rstd::task::Waker> m_waker;
        bool                            m_closed { false };
    };

    class NextEventFuture {
    public:
        using Output = LoopEvent;

        NextEventFuture(Impl& owner, Arc<rstd::async::Registration> registration, bool wait_write)
            : m_owner(&owner), m_registration(rstd::move(registration)), m_wait_write(wait_write) {}

        NextEventFuture(const NextEventFuture&)                    = delete;
        auto operator=(const NextEventFuture&) -> NextEventFuture& = delete;

        NextEventFuture(NextEventFuture&& other) noexcept
            : m_owner(rstd::exchange(other.m_owner, nullptr)),
              m_registration(rstd::move(other.m_registration)),
              m_wait_write(other.m_wait_write),
              m_read_waiter_id(rstd::exchange(other.m_read_waiter_id, 0)),
              m_write_waiter_id(rstd::exchange(other.m_write_waiter_id, 0)) {}

        auto operator=(NextEventFuture&& other) noexcept -> NextEventFuture& {
            if (this != &other) {
                cancel();
                m_owner           = rstd::exchange(other.m_owner, nullptr);
                m_registration    = rstd::move(other.m_registration);
                m_wait_write      = other.m_wait_write;
                m_read_waiter_id  = rstd::exchange(other.m_read_waiter_id, 0);
                m_write_waiter_id = rstd::exchange(other.m_write_waiter_id, 0);
            }
            return *this;
        }

        ~NextEventFuture() { cancel(); }

        auto poll(rstd::pin::Pin<rstd::mut_ref<NextEventFuture>> self, rstd::task::Context& cx)
            -> rstd::task::Poll<LoopEvent> {
            auto& future = *self.get_unchecked_mut();

            auto command = future.m_owner->m_commands.poll_receive(cx);
            if (command.is_ready()) {
                future.cancel_readiness();
                auto value = rstd::move(command).take();
                if (value.is_none()) {
                    return rstd::task::Poll<LoopEvent>::Ready(QueueClosed {});
                }
                return rstd::task::Poll<LoopEvent>::Ready(rstd::move(value).unwrap_unchecked());
            }

            if (! future.m_registration) {
                return rstd::task::Poll<LoopEvent>::Pending();
            }

            auto read = future.m_registration->poll_readiness(
                cx, rstd::async::Interest::readable(), future.m_read_waiter_id);
            if (read.is_ready()) {
                future.m_owner->m_commands.clear_waker();
                future.m_read_waiter_id = 0;
                future.clear_write_waker();

                auto value = rstd::move(read).take();
                if (value.is_err()) {
                    return rstd::task::Poll<LoopEvent>::Ready(
                        IoError { rstd::move(value).unwrap_err_unchecked() });
                }
                return rstd::task::Poll<LoopEvent>::Ready(Readable {});
            }

            if (future.m_wait_write) {
                auto write = future.m_registration->poll_readiness(
                    cx, rstd::async::Interest::writable(), future.m_write_waiter_id);
                if (write.is_ready()) {
                    future.m_owner->m_commands.clear_waker();
                    future.clear_read_waker();
                    future.m_write_waiter_id = 0;

                    auto value = rstd::move(write).take();
                    if (value.is_err()) {
                        return rstd::task::Poll<LoopEvent>::Ready(
                            IoError { rstd::move(value).unwrap_err_unchecked() });
                    }
                    return rstd::task::Poll<LoopEvent>::Ready(Writable {});
                }
            }

            return rstd::task::Poll<LoopEvent>::Pending();
        }

    private:
        void cancel() {
            if (m_owner) {
                m_owner->m_commands.clear_waker();
            }
            cancel_readiness();
            m_owner = nullptr;
        }

        void cancel_readiness() {
            clear_read_waker();
            clear_write_waker();
        }

        void clear_read_waker() {
            if (m_registration && m_read_waiter_id != 0) {
                m_registration->clear_waker(rstd::async::Interest::readable(), m_read_waiter_id);
                m_read_waiter_id = 0;
            }
        }

        void clear_write_waker() {
            if (m_registration && m_write_waiter_id != 0) {
                m_registration->clear_waker(rstd::async::Interest::writable(), m_write_waiter_id);
                m_write_waiter_id = 0;
            }
        }

        Impl*                          m_owner {};
        Arc<rstd::async::Registration> m_registration;
        bool                           m_wait_write { false };
        usize                          m_read_waiter_id {};
        usize                          m_write_waiter_id {};
    };

public:
    Impl(rstd::Option<u64> max_buffer_size, std::pmr::memory_resource* mem_pool)
        : m_alloc(mem_pool),
          m_read_buffer(max_buffer_size.unwrap_or(MaxBufferSize), m_alloc),
          m_msgs(m_alloc),
          m_curl(curl_easy_init()),
          m_worker([this] {
              worker_main();
          }) {}

    ~Impl() { stop_worker(); }

    auto connect(const std::string& url) -> std::future<bool> {
        auto promise = make_arc<std::promise<bool>>();
        auto future  = promise->get_future();
        if (! m_commands.push(ConnectCommand { url, promise })) {
            promise->set_value(false);
        }
        return future;
    }

    void disconnect() { (void)m_commands.push(DisconnectCommand { .send_close = true }); }

    auto is_connected() const -> bool { return m_connected.load(std::memory_order_acquire); }

    void send(std::string_view message) {
        send(std::span<const rstd::byte> { reinterpret_cast<const rstd::byte*>(message.data()),
                                           message.size() });
    }

    void send(std::span<const rstd::byte> in) {
        auto msg = rstd::rc::allocate_make_rc<rstd::byte[]>(m_alloc, in.size(), rstd::byte {});
        std::copy_n(in.begin(), in.size(), msg.get());
        (void)m_commands.push(SendCommand { rstd::move(msg) });
    }

    void set_on_connected_callback(ConnectedCallback callback) {
        auto lock      = std::lock_guard { m_callback_mutex };
        m_on_connected = rstd::move(callback);
    }

    void set_on_message_callback(MessageCallback callback) {
        auto lock    = std::lock_guard { m_callback_mutex };
        m_on_message = rstd::move(callback);
    }

    void set_on_error_callback(ErrorCallback callback) {
        auto lock  = std::lock_guard { m_callback_mutex };
        m_on_error = rstd::move(callback);
    }

    auto on_message_callback() -> const MessageCallback& { return m_on_message; }

private:
    void worker_main() {
        auto runtime = rstd::async::Runtime {};
        runtime.block_on(command_loop());
    }

    auto command_loop() -> coro<void> {
        for (;;) {
            if (is_connected()) {
                if (! read_available()) continue;
                if (! flush_write()) continue;
            }

            auto event = co_await NextEventFuture { *this, m_registration, ! m_msgs.empty() };
            if (! handle_event(rstd::move(event))) {
                break;
            }
        }

        close_connection(false, false);
        m_commands.close();
        co_return;
    }

    auto handle_event(LoopEvent event) -> bool {
        if (std::holds_alternative<QueueClosed>(event)) {
            return false;
        }

        if (auto* command = std::get_if<Command>(&event)) {
            return handle_command(rstd::move(*command));
        }

        if (std::holds_alternative<Readable>(event)) {
            (void)read_available();
            return true;
        }

        if (std::holds_alternative<Writable>(event)) {
            (void)flush_write();
            return true;
        }

        if (auto* error = std::get_if<IoError>(&event)) {
            if (is_connected()) {
                emit_io_error(rstd::move(error->error));
                close_connection(false, true);
            }
            return true;
        }

        return true;
    }

    auto handle_command(Command command) -> bool {
        if (auto* value = std::get_if<ConnectCommand>(&command)) {
            handle_connect(rstd::move(*value));
            return true;
        }

        if (auto* value = std::get_if<SendCommand>(&command)) {
            if (is_connected()) {
                m_msgs.emplace_back(rstd::move(value->message));
            }
            return true;
        }

        if (auto* value = std::get_if<DisconnectCommand>(&command)) {
            close_connection(value->send_close, true);
            return true;
        }

        return false;
    }

    void handle_connect(ConnectCommand command) {
        if (! m_curl) {
            m_curl = curl_easy_init();
        }

        if (! m_curl || is_connected()) {
            command.promise->set_value(false);
            return;
        }

        auto result = curl_easy_setopt(m_curl, CURLoption::CURLOPT_URL, command.url.c_str());
        if (result == CURLcode::CURLE_OK) {
            result = curl_easy_setopt(m_curl, CURLoption::CURLOPT_CONNECT_ONLY, 2L);
        }
        if (result == CURLcode::CURLE_OK) {
            result = curl_easy_perform(m_curl);
        }

        if (result != CURLcode::CURLE_OK) {
            emit_curl_error(result);
            close_connection(false, true);
            command.promise->set_value(false);
            return;
        }

        auto sockfd = static_cast<curl_socket_t>(-1);
        result      = curl_easy_getinfo(m_curl, CURLINFO::CURLINFO_ACTIVESOCKET, &sockfd);
        if (result != CURLcode::CURLE_OK || sockfd == static_cast<curl_socket_t>(-1)) {
            if (result == CURLcode::CURLE_OK) {
                result = CURLcode::CURLE_COULDNT_CONNECT;
            }
            emit_curl_error(result);
            close_connection(false, true);
            command.promise->set_value(false);
            return;
        }

        auto registration =
            rstd::async::Registration::register_fd(static_cast<rstd::sys::fd::RawFd>(sockfd));
        if (registration.is_err()) {
            emit_io_error(rstd::move(registration).unwrap_err_unchecked());
            close_connection(false, true);
            command.promise->set_value(false);
            return;
        }

        reset_states();
        m_registration =
            make_arc<rstd::async::Registration>(rstd::move(registration).unwrap_unchecked());
        m_connected.store(true, std::memory_order_release);

        command.promise->set_value(true);
        emit_connected();
    }

    auto read_available() -> bool {
        if (! m_curl || ! is_connected()) return false;

        for (;;) {
            auto  rlen   = usize { 0 };
            auto* meta   = static_cast<const struct curl_ws_frame*>(nullptr);
            auto* data   = m_read_buffer.data() + m_read_len;
            auto  size   = m_read_buffer.size() - m_read_len;
            auto  result = curl_ws_recv(m_curl, data, size, &rlen, &meta);

            m_read_len += rlen;
            if (result == CURLcode::CURLE_AGAIN) {
                clear_readiness(rstd::async::Ready::readable());
                return true;
            }
            if (result != CURLcode::CURLE_OK) {
                emit_curl_error(result);
                close_connection(false, true);
                return false;
            }

            if (meta != nullptr && (meta->flags & CURLWS_CLOSE) != 0) {
                close_connection(false, true);
                return false;
            }

            auto last = meta == nullptr || (! (meta->flags & CURLWS_CONT) && meta->bytesleft == 0);
            if (last || m_read_buffer.size() == m_read_len || rlen == 0) {
                emit_message(std::span<const rstd::byte> { m_read_buffer.data(), m_read_len },
                             last);
                m_read_len = 0;
            }

            if (! is_connected()) return false;
        }
    }

    auto flush_write() -> bool {
        if (! m_curl || ! is_connected()) return false;

        while (! m_msgs.empty()) {
            auto msg = m_msgs.front();

            for (;;) {
                auto sent   = usize { 0 };
                auto data   = msg.get() + m_sent_len;
                auto size   = msg.size() - m_sent_len;
                auto result = curl_ws_send(m_curl, data, size, &sent, 0, CURLWS_BINARY);

                m_sent_len += sent;
                if (result == CURLcode::CURLE_AGAIN) {
                    clear_readiness(rstd::async::Ready::writable());
                    return true;
                }
                if (result != CURLcode::CURLE_OK) {
                    emit_curl_error(result);
                    close_connection(false, true);
                    return false;
                }

                m_sent_len = 0;
                m_msgs.pop_front();
                break;
            }
        }

        return true;
    }

    void clear_readiness(rstd::async::Ready ready) {
        if (m_registration) {
            m_registration->clear_readiness(ready);
        }
    }

    void close_connection(bool send_close, bool recreate_easy) {
        auto was_connected = m_connected.exchange(false, std::memory_order_acq_rel);

        if (m_registration) {
            m_registration->reset();
            m_registration.reset();
        }

        if (m_curl) {
            if (was_connected && send_close) {
                (void)curl_ws_send(m_curl, "", 0, nullptr, 0, CURLWS_CLOSE);
            }
            curl_easy_cleanup(m_curl);
            m_curl = recreate_easy ? curl_easy_init() : nullptr;
        }

        reset_states();
    }

    void reset_states() {
        m_read_len = 0;
        m_sent_len = 0;
        m_msgs.clear();
    }

    void stop_worker() {
        auto expected = false;
        if (! m_stop_requested.compare_exchange_strong(expected, true)) {
            return;
        }

        if (! m_commands.push(StopCommand {})) {
            m_commands.close();
        }

        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    auto connected_callback() -> ConnectedCallback {
        auto lock = std::lock_guard { m_callback_mutex };
        return m_on_connected;
    }

    auto message_callback() -> MessageCallback {
        auto lock = std::lock_guard { m_callback_mutex };
        return m_on_message;
    }

    auto error_callback() -> ErrorCallback {
        auto lock = std::lock_guard { m_callback_mutex };
        return m_on_error;
    }

    void emit_connected() {
        auto callback = connected_callback();
        if (callback) callback();
    }

    void emit_message(std::span<const rstd::byte> data, bool last) {
        auto callback = message_callback();
        if (callback) callback(data, last);
    }

    void emit_error(rstd::ref<rstd::str> message) {
        auto callback = error_callback();
        if (callback) callback(message);
    }

    void emit_curl_error(CURLcode code) {
        m_error_message = rstd::format("{}({})", curl_easy_strerror(code), static_cast<int>(code));
        emit_error(m_error_message.as_str());
    }

    void emit_io_error(rstd::io::error::Error error) {
        m_error_message = rstd::format("{}", error);
        emit_error(m_error_message.as_str());
    }

    std::pmr::polymorphic_allocator<rstd::byte>       m_alloc;
    std::pmr::vector<rstd::byte>                      m_read_buffer;
    u64                                               m_read_len { 0 };
    std::pmr::deque<rstd::rc::Rc<const rstd::byte[]>> m_msgs;
    u64                                               m_sent_len { 0 };

    ::curl::CURL*                  m_curl {};
    Arc<rstd::async::Registration> m_registration;
    std::atomic_bool               m_connected { false };
    std::atomic_bool               m_stop_requested { false };
    CommandQueue                   m_commands;
    std::thread                    m_worker;

    std::mutex        m_callback_mutex;
    ConnectedCallback m_on_connected;
    MessageCallback   m_on_message;
    ErrorCallback     m_on_error;

    rstd::string::String m_error_message;
};

WebSocketBackend::WebSocketBackend(rstd::Option<u64>          max_buffer_size,
                                   std::pmr::memory_resource* mem_pool)
    : m_impl(make_box<Impl>(rstd::move(max_buffer_size), mem_pool)) {}

WebSocketBackend::~WebSocketBackend() = default;

auto WebSocketBackend::connect(const std::string& url) -> std::future<bool> {
    return m_impl->connect(url);
}

void WebSocketBackend::disconnect() { m_impl->disconnect(); }

bool WebSocketBackend::is_connected() const { return m_impl->is_connected(); }

void WebSocketBackend::send(std::string_view message) { m_impl->send(message); }

void WebSocketBackend::send(std::span<const rstd::byte> message) { m_impl->send(message); }

void WebSocketBackend::set_on_connected_callback(ConnectedCallback cb) {
    m_impl->set_on_connected_callback(rstd::move(cb));
}

void WebSocketBackend::set_on_message_callback(MessageCallback cb) {
    m_impl->set_on_message_callback(rstd::move(cb));
}

void WebSocketBackend::set_on_error_callback(ErrorCallback cb) {
    m_impl->set_on_error_callback(rstd::move(cb));
}

auto WebSocketBackend::on_message_callback() -> const MessageCallback& {
    return m_impl->on_message_callback();
}

} // namespace ncrequest::client::curl
