module;
#include <deque>
#include <memory_resource>
#include <rstd/enum.hpp>
#include <vector>

module ncrequest;
import :client_curl_websocket;
import rstd;

using namespace ::curl;

namespace ncrequest::client::curl
{
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

class WebSocketBackend::Impl {
    struct ConnectCommand {
        rstd::string::String                url;
        rstd::async::CompletionHandle<bool> completion;
    };

    struct SendCommand {
        rstd::rc::Rc<const rstd::byte[]> message;
    };

    struct DisconnectCommand {
        bool send_close { false };
    };

    struct StopCommand {};

#define NCREQUEST_CURL_WS_COMMAND_VARIANTS(V)       \
    V(Connect, (ConnectCommand value;))             \
    V(Send, (SendCommand value;))                   \
    V(Disconnect, (DisconnectCommand value;))       \
    V(Stop, ())

    struct Command {
        RSTD_ENUM_BODY(Command, NCREQUEST_CURL_WS_COMMAND_VARIANTS)
    };

#undef NCREQUEST_CURL_WS_COMMAND_VARIANTS

#define NCREQUEST_CURL_WS_LOOP_EVENT_VARIANTS(V) \
    V(QueueClosed, ())                           \
    V(Command, (Command value;))                 \
    V(Readable, ())                              \
    V(Writable, ())                              \
    V(IoError, (rstd::io::error::Error error;))

    struct LoopEvent {
        RSTD_ENUM_BODY(LoopEvent, NCREQUEST_CURL_WS_LOOP_EVENT_VARIANTS)
    };

#undef NCREQUEST_CURL_WS_LOOP_EVENT_VARIANTS

    class CommandQueue {
        struct Fields {
            rstd::vec::Vec<Command>       commands;
            rstd::Option<rstd::task::Waker> waker;
            bool                         closed { false };

            Fields(): commands(rstd::vec::Vec<Command>::make()) {}
        };

    public:
        using Output = rstd::Option<Command>;

        CommandQueue(): m_fields(Fields {}) {}

        auto push(Command command) -> rstd::Result<empty, Command> {
            auto waker = rstd::Option<rstd::task::Waker> {};
            {
                auto fields = m_fields.lock().unwrap();
                if (fields->closed) return rstd::Err(rstd::move(command));

                fields->commands.push(rstd::move(command));
                waker = fields->waker.take();
            }

            if (waker.is_some()) {
                rstd::move(*waker).wake();
            }
            return rstd::Ok(empty {});
        }

        void close() {
            auto waker = rstd::Option<rstd::task::Waker> {};
            {
                auto fields = m_fields.lock().unwrap();
                if (fields->closed) return;

                fields->closed = true;
                waker          = fields->waker.take();
            }

            if (waker.is_some()) {
                rstd::move(*waker).wake();
            }
        }

        void clear_waker() {
            auto fields  = m_fields.lock().unwrap();
            fields->waker = rstd::None();
        }

        auto poll_receive(rstd::task::Context& cx) -> rstd::task::Poll<Output> {
            auto fields = m_fields.lock().unwrap();
            if (! fields->commands.is_empty()) {
                auto command = fields->commands.remove(0);
                return rstd::task::Poll<Output>::Ready(rstd::Some(rstd::move(command)));
            }

            if (fields->closed) {
                return rstd::task::Poll<Output>::Ready(rstd::None<Command>());
            }

            fields->waker = rstd::Some(cx.waker().clone());
            return rstd::task::Poll<Output>::Pending();
        }

    private:
        rstd::sync::Mutex<Fields> m_fields;
    };

    class NextEventFuture {
    public:
        using Output = LoopEvent;

        NextEventFuture(Impl& owner,
                        rstd::Option<Arc<rstd::async::Registration>> registration,
                        bool wait_write)
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

        auto poll(rstd::mut_ref<NextEventFuture> self, rstd::task::Context& cx)
            -> rstd::task::Poll<LoopEvent> {
            auto& future = *self;

            auto command = future.m_owner->m_commands.poll_receive(cx);
            if (command.is_ready()) {
                future.cancel_readiness();
                auto value = rstd::move(command).take();
                if (value.is_none()) {
                    return rstd::task::Poll<LoopEvent>::Ready(LoopEvent::QueueClosed());
                }
                return rstd::task::Poll<LoopEvent>::Ready(
                    LoopEvent::Command(rstd::move(value).unwrap_unchecked()));
            }

            if (! future.m_registration) {
                return rstd::task::Poll<LoopEvent>::Pending();
            }

            auto read = (*future.m_registration)->poll_readiness(
                cx, rstd::async::Interest::readable(), future.m_read_waiter_id);
            if (read.is_ready()) {
                future.m_owner->m_commands.clear_waker();
                future.m_read_waiter_id = 0;
                future.clear_write_waker();

                auto value = rstd::move(read).take();
                if (value.is_err()) {
                    return rstd::task::Poll<LoopEvent>::Ready(
                        LoopEvent::IoError(rstd::move(value).unwrap_err_unchecked()));
                }
                return rstd::task::Poll<LoopEvent>::Ready(LoopEvent::Readable());
            }

            if (future.m_wait_write) {
                auto write = (*future.m_registration)->poll_readiness(
                    cx, rstd::async::Interest::writable(), future.m_write_waiter_id);
                if (write.is_ready()) {
                    future.m_owner->m_commands.clear_waker();
                    future.clear_read_waker();
                    future.m_write_waiter_id = 0;

                    auto value = rstd::move(write).take();
                    if (value.is_err()) {
                        return rstd::task::Poll<LoopEvent>::Ready(
                            LoopEvent::IoError(rstd::move(value).unwrap_err_unchecked()));
                    }
                    return rstd::task::Poll<LoopEvent>::Ready(LoopEvent::Writable());
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
                (*m_registration)
                    ->clear_waker(rstd::async::Interest::readable(), m_read_waiter_id);
                m_read_waiter_id = 0;
            }
        }

        void clear_write_waker() {
            if (m_registration && m_write_waiter_id != 0) {
                (*m_registration)
                    ->clear_waker(rstd::async::Interest::writable(), m_write_waiter_id);
                m_write_waiter_id = 0;
            }
        }

        Impl*                          m_owner {};
        rstd::Option<Arc<rstd::async::Registration>> m_registration;
        bool                           m_wait_write { false };
        usize                          m_read_waiter_id {};
        usize                          m_write_waiter_id {};
    };

    struct Callbacks {
        ConnectedCallback    connected;
        DisconnectedCallback disconnected;
        MessageCallback      message;
        ErrorCallback        error;
    };

public:
    Impl(rstd::Option<u64> max_buffer_size, std::pmr::memory_resource* mem_pool)
        : m_alloc(mem_pool),
          m_read_buffer(max_buffer_size.unwrap_or(MaxBufferSize), m_alloc),
          m_msgs(m_alloc),
          m_curl(curl_easy_init()),
          m_connected(false),
          m_stop_requested(false),
          m_callbacks(Callbacks {}) {
        auto worker = rstd::thread::spawn([this] {
            worker_main();
        });
        if (worker.is_err()) rstd::panic { "failed to start curl WebSocket worker" };
        m_worker = Some(rstd::move(worker).unwrap());
    }

    ~Impl() { stop_worker(); }

    auto connect(ref<str> url) -> rstd::async::Completion<bool> {
        auto made       = rstd::async::Completion<bool>::make();
        auto pair       = rstd::move(made).unwrap();
        auto completion = rstd::move(pair.get<0>());
        auto command    = Command::Connect(
            ConnectCommand { rstd::string::String::make(url), rstd::move(pair.get<1>()) });
        auto pushed = m_commands.push(rstd::move(command));
        if (pushed.is_err()) {
            auto rejected = rstd::move(pushed).unwrap_err();
            (void)rstd::move(rejected).as_Connect().value.completion.complete(false);
        }
        return completion;
    }

    void disconnect() {
        (void)m_commands.push(Command::Disconnect(DisconnectCommand { .send_close = true }));
    }

    auto is_connected() const -> bool { return m_connected.load(Ordering::Acquire); }

    void send(ref<str> message) {
        send(slice<byte>::from_raw_parts(
            reinterpret_cast<const byte*>(message.data()), message.size()));
    }

    void send(slice<byte> in) {
        auto msg = rstd::rc::allocate_make_rc<byte[]>(m_alloc, in.len(), byte {});
        rstd::mem::memcpy(msg.get(), in.as_raw_ptr(), in.len());
        (void)m_commands.push(Command::Send(SendCommand { rstd::move(msg) }));
    }

    void set_on_connected_callback(ConnectedCallback callback) {
        auto callbacks       = m_callbacks.lock().unwrap();
        callbacks->connected = rstd::move(callback);
    }

    void set_on_disconnected_callback(DisconnectedCallback callback) {
        auto callbacks          = m_callbacks.lock().unwrap();
        callbacks->disconnected = rstd::move(callback);
    }

    void set_on_message_callback(MessageCallback callback) {
        auto callbacks     = m_callbacks.lock().unwrap();
        callbacks->message = rstd::move(callback);
    }

    void set_on_error_callback(ErrorCallback callback) {
        auto callbacks   = m_callbacks.lock().unwrap();
        callbacks->error = rstd::move(callback);
    }

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

            auto registration = m_registration.is_some()
                                    ? rstd::Some(m_registration->clone())
                                    : rstd::None<Arc<rstd::async::Registration>>();
            auto event = co_await NextEventFuture {
                *this, rstd::move(registration), ! m_msgs.empty()
            };
            if (! handle_event(rstd::move(event))) {
                break;
            }
        }

        close_connection(false, false);
        m_commands.close();
        co_return;
    }

    auto handle_event(LoopEvent event) -> bool {
        if (event.is_QueueClosed()) {
            return false;
        }

        if (event.is_Command()) {
            return handle_command(rstd::move(event).as_Command().value);
        }

        if (event.is_Readable()) {
            (void)read_available();
            return true;
        }

        if (event.is_Writable()) {
            (void)flush_write();
            return true;
        }

        if (event.is_IoError()) {
            auto error = rstd::move(event).as_IoError().error;
            if (is_connected()) {
                emit_io_error(rstd::move(error));
                close_connection(false, true);
            }
            return true;
        }

        return true;
    }

    auto handle_command(Command command) -> bool {
        if (command.is_Connect()) {
            handle_connect(rstd::move(command).as_Connect().value);
            return true;
        }

        if (command.is_Send()) {
            auto value = rstd::move(command).as_Send().value;
            if (is_connected()) {
                m_msgs.emplace_back(rstd::move(value.message));
            }
            return true;
        }

        if (command.is_Disconnect()) {
            auto value = rstd::move(command).as_Disconnect().value;
            close_connection(value.send_close, true);
            return true;
        }

        return false;
    }

    void handle_connect(ConnectCommand command) {
        if (! m_curl) {
            m_curl = curl_easy_init();
        }

        if (! m_curl || is_connected()) {
            (void)command.completion.complete(false);
            return;
        }

        auto url    = rstd::ffi::CString::from_vec_unchecked(
            rstd::into<rstd::vec::Vec<u8>>(rstd::move(command.url)));
        auto result = curl_easy_setopt(
            m_curl,
            CURLoption::CURLOPT_URL,
            reinterpret_cast<const char*>(url.to_bytes_with_nul().as_raw_ptr()));
        if (result == CURLcode::CURLE_OK) {
            result = curl_easy_setopt(m_curl, CURLoption::CURLOPT_CONNECT_ONLY, 2L);
        }
        if (result == CURLcode::CURLE_OK) {
            result = curl_easy_perform(m_curl);
        }

        if (result != CURLcode::CURLE_OK) {
            emit_curl_error(result);
            close_connection(false, true);
            (void)command.completion.complete(false);
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
            (void)command.completion.complete(false);
            return;
        }

        auto registration =
            rstd::async::Registration::register_fd(static_cast<rstd::sys::fd::RawFd>(sockfd));
        if (registration.is_err()) {
            emit_io_error(rstd::move(registration).unwrap_err_unchecked());
            close_connection(false, true);
            (void)command.completion.complete(false);
            return;
        }

        reset_states();
        m_registration = rstd::Some(Arc<rstd::async::Registration>::make(
            rstd::move(registration).unwrap_unchecked()));
        m_connected.store(true, Ordering::Release);

        (void)command.completion.complete(true);
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
                emit_message(slice<byte>::from_raw_parts(m_read_buffer.data(), m_read_len), last);
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
            (*m_registration)->clear_readiness(ready);
        }
    }

    void close_connection(bool send_close, bool recreate_easy) {
        auto was_connected = m_connected.exchange(false, Ordering::AcqRel);

        if (m_registration) {
            (*m_registration)->reset();
            m_registration = rstd::None();
        }

        if (m_curl) {
            if (was_connected && send_close) {
                (void)curl_ws_send(m_curl, "", 0, nullptr, 0, CURLWS_CLOSE);
            }
            curl_easy_cleanup(m_curl);
            m_curl = recreate_easy ? curl_easy_init() : nullptr;
        }

        reset_states();
        if (was_connected) emit_disconnected();
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

        if (m_commands.push(Command::Stop()).is_err()) {
            m_commands.close();
        }

        auto worker = m_worker.take();
        if (worker.is_some()) {
            (void)rstd::move(*worker).join();
        }
    }

    auto connected_callback() -> ConnectedCallback {
        auto callbacks = m_callbacks.lock().unwrap();
        return callbacks->connected.clone();
    }

    auto message_callback() -> MessageCallback {
        auto callbacks = m_callbacks.lock().unwrap();
        return callbacks->message.clone();
    }

    auto disconnected_callback() -> DisconnectedCallback {
        auto callbacks = m_callbacks.lock().unwrap();
        return callbacks->disconnected.clone();
    }

    auto error_callback() -> ErrorCallback {
        auto callbacks = m_callbacks.lock().unwrap();
        return callbacks->error.clone();
    }

    void emit_connected() {
        auto callback = connected_callback();
        if (callback) callback();
    }

    void emit_disconnected() {
        auto callback = disconnected_callback();
        if (callback) callback();
    }

    void emit_message(slice<byte> data, bool last) {
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
    rstd::Option<Arc<rstd::async::Registration>> m_registration;
    Atomic<bool>                   m_connected;
    Atomic<bool>                   m_stop_requested;
    CommandQueue                   m_commands;
    Option<rstd::thread::JoinHandle<void>> m_worker;

    rstd::sync::Mutex<Callbacks> m_callbacks;

    rstd::string::String m_error_message;
};

WebSocketBackend::WebSocketBackend(rstd::Option<u64>          max_buffer_size,
                                   std::pmr::memory_resource* mem_pool)
    : m_impl(Box<Impl>::make(rstd::move(max_buffer_size), mem_pool)) {}

WebSocketBackend::~WebSocketBackend() = default;

auto WebSocketBackend::connect(ref<str> url) -> rstd::async::Completion<bool> {
    return m_impl->connect(url);
}

void WebSocketBackend::disconnect() { m_impl->disconnect(); }

bool WebSocketBackend::is_connected() const { return m_impl->is_connected(); }

void WebSocketBackend::send(ref<str> message) { m_impl->send(message); }

void WebSocketBackend::send(slice<byte> message) { m_impl->send(message); }

void WebSocketBackend::set_on_connected_callback(ConnectedCallback cb) {
    m_impl->set_on_connected_callback(rstd::move(cb));
}

void WebSocketBackend::set_on_disconnected_callback(DisconnectedCallback cb) {
    m_impl->set_on_disconnected_callback(rstd::move(cb));
}

void WebSocketBackend::set_on_message_callback(MessageCallback cb) {
    m_impl->set_on_message_callback(rstd::move(cb));
}

void WebSocketBackend::set_on_error_callback(ErrorCallback cb) {
    m_impl->set_on_error_callback(rstd::move(cb));
}

} // namespace ncrequest::client::curl
