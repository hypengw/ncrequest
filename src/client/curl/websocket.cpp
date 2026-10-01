module;
#include <rstd/enum.hpp>

module ncrequest;
import :client_curl_websocket;
import rstd;

using namespace rstd::prelude;
using namespace ::curl;
using rstd::sync::Arc;
using IoError = rstd::io::error::Error;
using alloc::collections::VecDeque;
using rstd::async::Completion;
using rstd::async::CompletionHandle;
using rstd::async::Interest;
using rstd::async::Ready;
using rstd::async::Registration;
using rstd::async::Runtime;
using rstd::bytes::Bytes;
using rstd::ffi::CString;
using rstd::os::fd::RawFd;
using rstd::sync::Mutex;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;
using rstd::task::Context;
using rstd::task::Poll;
using rstd::task::Waker;
using rstd::thread::JoinHandle;
using rstd::thread::spawn;

namespace ncrequest::client::curl
{

class WebSocketBackend::Impl {
    struct ConnectCommand {
        String                 url;
        CompletionHandle<bool> completion;
    };

    struct SendCommand {
        Bytes message;
    };

    struct DisconnectCommand {
        bool send_close { false };
    };

    struct StopCommand {};

    struct Command {
        RSTD_ENUM(Command, (Connect, (ConnectCommand value;)), (Send, (SendCommand value;)),
                  (Disconnect, (DisconnectCommand value;)), (Stop))
    };

    struct LoopEvent {
        RSTD_ENUM(LoopEvent, (QueueClosed), (Command, (Command value;)), (Readable), (Writable),
                  (IoError, (IoError error;)))
    };

    class CommandQueue {
        struct Fields {
            Vec<Command>  commands;
            Option<Waker> waker;
            bool          closed { false };

            Fields(): commands(Vec<Command>::make()) {}
        };

    public:
        using Output = Option<Command>;

        CommandQueue(): m_fields(Fields {}) {}

        auto push(Command command) -> rstd::Result<empty, Command> {
            auto waker = Option<Waker> {};
            {
                auto fields = m_fields.lock().unwrap();
                if (fields->closed) return Err(rstd::move(command));

                fields->commands.push(rstd::move(command));
                waker = fields->waker.take();
            }

            if (waker.is_some()) {
                rstd::move(*waker).wake();
            }
            return Ok(empty {});
        }

        void close() {
            auto waker = Option<Waker> {};
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
            auto fields   = m_fields.lock().unwrap();
            fields->waker = None();
        }

        auto poll_receive(Context& cx) -> Poll<Output> {
            auto fields = m_fields.lock().unwrap();
            if (! fields->commands.is_empty()) {
                auto command = fields->commands.remove(usize());
                return Poll<Output>::Ready(Some(rstd::move(command)));
            }

            if (fields->closed) {
                return Poll<Output>::Ready(None<Command>());
            }

            fields->waker = Some(cx.waker().clone());
            return Poll<Output>::Pending();
        }

    private:
        Mutex<Fields> m_fields;
    };

    class NextEventFuture {
    public:
        using Output = LoopEvent;

        NextEventFuture(Impl& owner, Option<Arc<Registration>> registration, bool wait_write)
            : m_owner(&owner), m_registration(rstd::move(registration)), m_wait_write(wait_write) {}

        NextEventFuture(const NextEventFuture&)                    = delete;
        auto operator=(const NextEventFuture&) -> NextEventFuture& = delete;

        NextEventFuture(NextEventFuture&& other) noexcept
            : m_owner(rstd::exchange(other.m_owner, nullptr)),
              m_registration(rstd::move(other.m_registration)),
              m_wait_write(other.m_wait_write),
              m_read_waiter_id(rstd::exchange(other.m_read_waiter_id, usize())),
              m_write_waiter_id(rstd::exchange(other.m_write_waiter_id, usize())) {}

        auto operator=(NextEventFuture&& other) noexcept -> NextEventFuture& {
            if (this != &other) {
                cancel();
                m_owner           = rstd::exchange(other.m_owner, nullptr);
                m_registration    = rstd::move(other.m_registration);
                m_wait_write      = other.m_wait_write;
                m_read_waiter_id  = rstd::exchange(other.m_read_waiter_id, usize());
                m_write_waiter_id = rstd::exchange(other.m_write_waiter_id, usize());
            }
            return *this;
        }

        ~NextEventFuture() { cancel(); }

        auto poll(mut_ref<NextEventFuture> self, Context& cx) -> Poll<LoopEvent> {
            auto& future = *self;

            auto command = future.m_owner->m_commands.poll_receive(cx);
            if (command.is_ready()) {
                future.cancel_readiness();
                auto value = rstd::move(command).take();
                if (value.is_none()) {
                    return Poll<LoopEvent>::Ready(LoopEvent::QueueClosed());
                }
                return Poll<LoopEvent>::Ready(
                    LoopEvent::Command(rstd::move(value).unwrap_unchecked()));
            }

            if (! future.m_registration) {
                return Poll<LoopEvent>::Pending();
            }

            auto read = (*future.m_registration)
                            ->poll_readiness(cx, Interest::readable(), future.m_read_waiter_id);
            if (read.is_ready()) {
                future.m_owner->m_commands.clear_waker();
                future.m_read_waiter_id = usize();
                future.clear_write_waker();

                auto value = rstd::move(read).take();
                if (value.is_err()) {
                    return Poll<LoopEvent>::Ready(
                        LoopEvent::IoError(rstd::move(value).unwrap_err_unchecked()));
                }
                return Poll<LoopEvent>::Ready(LoopEvent::Readable());
            }

            if (future.m_wait_write) {
                auto write =
                    (*future.m_registration)
                        ->poll_readiness(cx, Interest::writable(), future.m_write_waiter_id);
                if (write.is_ready()) {
                    future.m_owner->m_commands.clear_waker();
                    future.clear_read_waker();
                    future.m_write_waiter_id = usize();

                    auto value = rstd::move(write).take();
                    if (value.is_err()) {
                        return Poll<LoopEvent>::Ready(
                            LoopEvent::IoError(rstd::move(value).unwrap_err_unchecked()));
                    }
                    return Poll<LoopEvent>::Ready(LoopEvent::Writable());
                }
            }

            return Poll<LoopEvent>::Pending();
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
            if (m_registration && m_read_waiter_id != usize()) {
                (*m_registration)->clear_waker(Interest::readable(), m_read_waiter_id);
                m_read_waiter_id = usize();
            }
        }

        void clear_write_waker() {
            if (m_registration && m_write_waiter_id != usize()) {
                (*m_registration)->clear_waker(Interest::writable(), m_write_waiter_id);
                m_write_waiter_id = usize();
            }
        }

        Impl*                     m_owner {};
        Option<Arc<Registration>> m_registration;
        bool                      m_wait_write { false };
        usize                     m_read_waiter_id {};
        usize                     m_write_waiter_id {};
    };

    struct Callbacks {
        ConnectedCallback    connected;
        DisconnectedCallback disconnected;
        MessageCallback      message;
        ErrorCallback        error;
    };

public:
    Impl(Option<u64> max_buffer_size)
        : m_curl(curl_easy_init()),
          m_connected(false),
          m_stop_requested(false),
          m_callbacks(Callbacks {}) {
        m_read_buffer.resize(as_cast<usize>(max_buffer_size.unwrap_or(MaxBufferSize)), u8());
        auto worker = spawn([this] {
            worker_main();
        });
        if (worker.is_err()) rstd::panic { "failed to start curl WebSocket worker" };
        m_worker = Some(rstd::move(worker).unwrap());
    }

    ~Impl() { stop_worker(); }

    auto connect(ref<str> url) -> Completion<bool> {
        auto made       = Completion<bool>::make();
        auto pair       = rstd::move(made).unwrap();
        auto completion = rstd::move(pair.get<0>());
        auto command =
            Command::Connect(ConnectCommand { String::make(url), rstd::move(pair.get<1>()) });
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

    void send(ref<str> message) { send(message.as_bytes()); }

    void send(slice<u8> in) {
        auto msg = Bytes::copy_from_slice(in);
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
        auto runtime = Runtime {};
        runtime.block_on(command_loop());
    }

    auto command_loop() -> coro<void> {
        for (;;) {
            if (is_connected()) {
                if (! read_available()) continue;
                if (! flush_write()) continue;
            }

            auto registration = m_registration.is_some() ? Some(m_registration->clone())
                                                         : None<Arc<Registration>>();
            auto event = co_await NextEventFuture { *this,
                                                    rstd::move(registration),
                                                    (m_pending.is_some() || ! m_msgs.is_empty()) };
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
                m_msgs.push_back(rstd::move(value.message));
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

        auto url    = CString::from_vec_unchecked(rstd::into<Vec<u8>>(rstd::move(command.url)));
        auto result = curl_easy_setopt(m_curl, CURLoption::CURLOPT_URL, url.as_ptr());
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

        auto registration = Registration::register_fd(static_cast<RawFd>(sockfd));
        if (registration.is_err()) {
            emit_io_error(rstd::move(registration).unwrap_err_unchecked());
            close_connection(false, true);
            (void)command.completion.complete(false);
            return;
        }

        reset_states();
        m_registration = Some(Arc<Registration>::make(rstd::move(registration).unwrap_unchecked()));
        m_connected.store(true, Ordering::Release);

        (void)command.completion.complete(true);
        emit_connected();
    }

    auto read_available() -> bool {
        if (! m_curl || ! is_connected()) return false;

        for (;;) {
            rstd::size_t rlen {};
            auto*        meta   = static_cast<const struct curl_ws_frame*>(nullptr);
            auto*        data   = m_read_buffer.data() + m_read_len;
            auto         size   = m_read_buffer.len().to_primitive() - m_read_len;
            auto         result = curl_ws_recv(m_curl, data, size, &rlen, &meta);

            m_read_len += rlen;
            if (result == CURLcode::CURLE_AGAIN) {
                clear_readiness(Ready::readable());
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
            if (last || m_read_buffer.len().to_primitive() == m_read_len || rlen == 0) {
                emit_message(slice<u8>::from_raw_parts(m_read_buffer.data(), usize(m_read_len)),
                             last);
                m_read_len = 0;
            }

            if (! is_connected()) return false;
        }
    }

    auto flush_write() -> bool {
        if (! m_curl || ! is_connected()) return false;

        while ((m_pending.is_some() || ! m_msgs.is_empty())) {
            if (m_pending.is_none()) m_pending = m_msgs.pop_front();
            auto& msg = *m_pending;

            for (;;) {
                rstd::size_t sent {};
                auto         data   = msg.data() + m_sent_len;
                auto         size   = msg.size().to_primitive() - m_sent_len;
                auto         result = curl_ws_send(m_curl, data, size, &sent, 0, CURLWS_BINARY);

                m_sent_len += sent;
                if (result == CURLcode::CURLE_AGAIN) {
                    clear_readiness(Ready::writable());
                    return true;
                }
                if (result != CURLcode::CURLE_OK) {
                    emit_curl_error(result);
                    close_connection(false, true);
                    return false;
                }

                m_sent_len = 0;
                m_pending  = None();
                break;
            }
        }

        return true;
    }

    void clear_readiness(Ready ready) {
        if (m_registration) {
            (*m_registration)->clear_readiness(ready);
        }
    }

    void close_connection(bool send_close, bool recreate_easy) {
        auto was_connected = m_connected.exchange(false, Ordering::AcqRel);

        if (m_registration) {
            (*m_registration)->reset();
            m_registration = None();
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
        m_pending = None();
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

    void emit_message(slice<u8> data, bool last) {
        auto callback = message_callback();
        if (callback) callback(data, last);
    }

    void emit_error(ref<str> message) {
        auto callback = error_callback();
        if (callback) callback(message);
    }

    void emit_curl_error(CURLcode code) {
        m_error_message = rstd::format("{}({})", curl_easy_strerror(code), static_cast<int>(code));
        emit_error(m_error_message.as_str());
    }

    void emit_io_error(IoError error) {
        m_error_message = rstd::format("{}", error);
        emit_error(m_error_message.as_str());
    }

    Vec<u8>         m_read_buffer;
    rstd::size_t    m_read_len {};
    VecDeque<Bytes> m_msgs;
    rstd::size_t    m_sent_len {};
    Option<Bytes>   m_pending;

    ::curl::CURL*             m_curl {};
    Option<Arc<Registration>> m_registration;
    Atomic<bool>              m_connected;
    Atomic<bool>              m_stop_requested;
    CommandQueue              m_commands;
    Option<JoinHandle<void>>  m_worker;

    Mutex<Callbacks> m_callbacks;

    String m_error_message;
};

WebSocketBackend::WebSocketBackend(Option<u64> max_buffer_size)
    : m_impl(Box<Impl>::make(rstd::move(max_buffer_size))) {}

WebSocketBackend::~WebSocketBackend() = default;

auto WebSocketBackend::connect(ref<str> url) -> Completion<bool> { return m_impl->connect(url); }

void WebSocketBackend::disconnect() { m_impl->disconnect(); }

bool WebSocketBackend::is_connected() const { return m_impl->is_connected(); }

void WebSocketBackend::send(ref<str> message) { m_impl->send(message); }

void WebSocketBackend::send(slice<u8> message) { m_impl->send(message); }

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
