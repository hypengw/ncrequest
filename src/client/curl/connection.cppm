module;
#include "log.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <rstd/enum.hpp>

export module ncrequest:client_curl_connection;
export import ncrequest.type;
export import ncrequest.curl;
export import ncrequest.coro;
export import :http;
export import :request;
export import :error;

using namespace ::curl;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

namespace ncrequest::client::curl
{
export class SessionBackend;

export class Connection;
namespace session_message
{
enum class Action
{
    Add,
    Cancel,
    PauseRecv,
    UnPauseRecv,
    PauseSend,
    UnPauseSend,
};

#define NCREQUEST_SESSION_MESSAGE_VARIANTS(V) \
    V(Stop, ())                               \
    V(ConnectAction, (Arc<Connection> con; Action action;))

RSTD_ENUM_WITH_DEFAULT(Message, NCREQUEST_SESSION_MESSAGE_VARIANTS, Stop)

#undef NCREQUEST_SESSION_MESSAGE_VARIANTS
} // namespace session_message

export using SessionMessage = session_message::Message;

export class SessionChannel : public NoCopy {
public:
    using WakeCallback = std::function<void()>;

    void set_wake_callback(WakeCallback callback) {
        auto lock = std::lock_guard { m_mutex };
        m_wake    = rstd::move(callback);
    }

    auto try_send(SessionMessage msg) -> bool {
        WakeCallback wake;
        {
            auto lock = std::lock_guard { m_mutex };
            m_messages.emplace_back(rstd::move(msg));
            wake = m_wake;
        }
        m_cv.notify_one();
        if (wake) wake();
        return true;
    }

    auto try_receive(SessionMessage& out) -> bool {
        auto lock = std::lock_guard { m_mutex };
        if (m_messages.empty()) return false;

        out = rstd::move(m_messages.front());
        m_messages.pop_front();
        return true;
    }

    auto receive() -> SessionMessage {
        auto lock = std::unique_lock { m_mutex };
        m_cv.wait(lock, [this] {
            return ! m_messages.empty();
        });
        auto out = rstd::move(m_messages.front());
        m_messages.pop_front();
        return out;
    }

private:
    std::mutex                 m_mutex;
    std::condition_variable    m_cv;
    std::deque<SessionMessage> m_messages;
    WakeCallback               m_wake;
};

export class Connection : public std::enable_shared_from_this<Connection> {
    friend class SessionBackend;

public:
    using allocator_type = std::pmr::polymorphic_allocator<char>;

    static constexpr usize RECV_LIMIT { 64 * 1024 };
    static constexpr usize SEND_LIMIT { 64 * 1024 };

    enum class State
    {
        NotStarted,
        Transfering,
        Canceled,
        Finished,
    };

    struct IoResult {
        rstd::Option<Error> error;
        bool                eof { false };
        usize               size { 0 };

        static auto ok(usize size) -> IoResult { return { None<Error>(), false, size }; }
        static auto done() -> IoResult { return { None<Error>(), true, 0 }; }
        static auto fail(Error error) -> IoResult {
            return { Some(rstd::move(error)), false, 0 };
        }
    };

    template<typename Allocator>
    class Buffer {
    public:
        Buffer(usize limit, const Allocator& aloc)
            : m_state(State::Empty), m_limit(limit), m_transferred(0), m_alloc(aloc) {}

        enum class State : i32
        {
            Empty = 0,
            Normal,
            Full,
        };

        bool is_full() const { return m_state.load() == State::Full; }
        bool empty() const { return m_state.load() == State::Empty; }

        auto size() const { return m_buf.size(); }
        auto data() const { return m_buf.data(); }

        auto commit(slice<u8> in) {
            auto copied = in.len();
            m_buf.extend_from_slice(in);
            m_transferred += copied;
            check_full();
            return copied;
        }

        auto commit(const u8* in, usize size) {
            return commit(slice<u8>::from_raw_parts(in, size));
        }

        auto consume(rstd::bytes::BytesMut& out) {
            auto chunk  = out.chunk_mut();
            auto copied = rstd::min(chunk.len(), m_buf.size());
            if (copied == 0) return usize { 0 };

            rstd::mem::memcpy(chunk.as_raw_ptr(), m_buf.data(), copied);
            out.advance_mut(copied);
            m_buf.advance(copied);
            check_full();
            return copied;
        }

        auto consume(u8* out, usize size) {
            auto copied = rstd::min(size, m_buf.size());
            if (copied == 0) return usize { 0 };

            rstd::mem::memcpy(out, m_buf.data(), copied);
            m_buf.advance(copied);
            check_full();
            return copied;
        }

        auto commit(rstd::bytes::Bytes& in) {
            auto chunk  = in.chunk();
            auto copied = commit(chunk);
            in.advance(copied);
            return copied;
        }

        auto allocator() const { return m_alloc; }

    private:
        void check_full() {
            auto s = size();
            m_state.store(s == 0 ? State::Empty : (s > m_limit ? State::Full : State::Normal));
        }

        rstd::bytes::BytesMut m_buf;
        Atomic<State>         m_state;
        usize                 m_limit;
        usize                 m_transferred;
        Allocator             m_alloc;
    };

    Connection(Arc<SessionChannel> session_channel, allocator_type allocator)
        : m_finish_ec(CURLcode::CURLE_OK),
          m_state(State::NotStarted),
          m_recv_paused(false),
          m_send_paused(false),
          m_easy(std::make_unique<CurlEasy>()),
          m_session_channel(rstd::move(session_channel)),
          m_recv_buf(RECV_LIMIT, allocator),
          m_send_buf(SEND_LIMIT, allocator) {
        auto& easy = *m_easy;
        easy.setopt(CURLoption::CURLOPT_WRITEFUNCTION, Connection::write_callback);
        easy.setopt(CURLoption::CURLOPT_WRITEDATA, this);

        easy.setopt(CURLoption::CURLOPT_HEADERFUNCTION, Connection::header_callback);
        easy.setopt(CURLoption::CURLOPT_HEADERDATA, this);

        easy.setopt(CURLoption::CURLOPT_READFUNCTION, Connection::read_callback);
        easy.setopt(CURLoption::CURLOPT_READDATA, this);
        easy.setopt(CURLoption::CURLOPT_PRIVATE, this);
    }

    auto get_arc() { return shared_from_this(); }

    auto& easy() { return *m_easy; }
    auto& easy() const { return *m_easy; }
    auto& channel() { return m_session_channel; }

    auto& header() const { return m_header; }
    auto& cookie_jar() const { return m_cookie_jar; }
    auto& url() const { return m_url; }
    void  set_url(std::string_view v) { m_url = v; }
    void  set_send_callback(const req_opt::Read::Callback& cb) { m_send_callback = cb; }

    auto is_finished() const -> bool {
        auto lock = std::lock_guard { m_mutex };
        return m_state == State::Finished || m_state == State::Canceled;
    }

    using Action = session_message::Action;
    void send_action(Action v) {
        auto msg = SessionMessage::ConnectAction(get_arc(), v);
        m_session_channel->try_send(rstd::move(msg));
    }

    void about_to_cancel() {
        auto state = State::NotStarted;
        {
            auto lock = std::lock_guard { m_mutex };
            state     = m_state;
        }
        if (state == State::Canceled || state == State::Finished) return;

        auto msg = SessionMessage::ConnectAction(get_arc(), session_message::Action::Cancel);
        m_session_channel->try_send(rstd::move(msg));
    }

    class ReadSomeFuture {
    public:
        using Output = IoResult;

        ReadSomeFuture(Arc<Connection> connection, rstd::bytes::BytesMut& buffer)
            : m_connection(rstd::move(connection)),
              m_buffer(&buffer),
              m_state(rstd_coro::make_poll_state<IoResult>()) {}

        ReadSomeFuture(const ReadSomeFuture&)            = delete;
        ReadSomeFuture& operator=(const ReadSomeFuture&) = delete;

        ReadSomeFuture(ReadSomeFuture&& other) noexcept
            : m_connection(rstd::move(other.m_connection)),
              m_buffer(other.m_buffer),
              m_state(rstd::move(other.m_state)),
              m_started(other.m_started) {}

        auto operator=(ReadSomeFuture&& other) noexcept -> ReadSomeFuture& {
            if (this != &other) {
                cancel();
                m_connection = rstd::move(other.m_connection);
                m_buffer     = other.m_buffer;
                m_state      = rstd::move(other.m_state);
                m_started    = other.m_started;
            }
            return *this;
        }

        ~ReadSomeFuture() { cancel(); }

        auto poll(rstd::pin::Pin<rstd::mut_ref<ReadSomeFuture>> self, rstd::task::Context& cx)
            -> rstd::task::Poll<IoResult> {
            auto& future = *self.get_unchecked_mut();
            if (! future.m_started) {
                future.m_started = true;
                future.m_connection->start_read_some(*future.m_buffer, future.m_state);
            }
            return future.m_state->poll(cx);
        }

    private:
        void cancel() {
            if (! m_state) return;
            m_state->cancel();
            if (m_connection) {
                m_connection->cancel_read_some(m_state);
            }
            m_state.reset();
            m_connection.reset();
        }

        Arc<Connection>                   m_connection;
        rstd::bytes::BytesMut*            m_buffer;
        rstd_coro::PollStateArc<IoResult> m_state;
        bool                              m_started { false };
    };

    class WriteSomeFuture {
    public:
        using Output = IoResult;

        WriteSomeFuture(Arc<Connection> connection, rstd::bytes::Bytes& buffer)
            : m_connection(rstd::move(connection)),
              m_buffer(&buffer),
              m_state(rstd_coro::make_poll_state<IoResult>()) {}

        WriteSomeFuture(const WriteSomeFuture&)            = delete;
        WriteSomeFuture& operator=(const WriteSomeFuture&) = delete;

        WriteSomeFuture(WriteSomeFuture&& other) noexcept
            : m_connection(rstd::move(other.m_connection)),
              m_buffer(other.m_buffer),
              m_state(rstd::move(other.m_state)),
              m_started(other.m_started) {}

        auto operator=(WriteSomeFuture&& other) noexcept -> WriteSomeFuture& {
            if (this != &other) {
                cancel();
                m_connection = rstd::move(other.m_connection);
                m_buffer     = other.m_buffer;
                m_state      = rstd::move(other.m_state);
                m_started    = other.m_started;
            }
            return *this;
        }

        ~WriteSomeFuture() { cancel(); }

        auto poll(rstd::pin::Pin<rstd::mut_ref<WriteSomeFuture>> self, rstd::task::Context& cx)
            -> rstd::task::Poll<IoResult> {
            auto& future = *self.get_unchecked_mut();
            if (! future.m_started) {
                future.m_started = true;
                future.m_connection->start_write_some(*future.m_buffer, future.m_state);
            }
            return future.m_state->poll(cx);
        }

    private:
        void cancel() {
            if (! m_state) return;
            m_state->cancel();
            if (m_connection) {
                m_connection->cancel_write_some(m_state);
            }
            m_state.reset();
            m_connection.reset();
        }

        Arc<Connection>                   m_connection;
        rstd::bytes::Bytes*               m_buffer;
        rstd_coro::PollStateArc<IoResult> m_state;
        bool                              m_started { false };
    };

    class WaitHeaderFuture {
    public:
        using Output = rstd::Option<Error>;

        explicit WaitHeaderFuture(Arc<Connection> connection)
            : m_connection(rstd::move(connection)),
              m_state(rstd_coro::make_poll_state<rstd::Option<Error>>()) {}

        WaitHeaderFuture(const WaitHeaderFuture&)            = delete;
        WaitHeaderFuture& operator=(const WaitHeaderFuture&) = delete;

        WaitHeaderFuture(WaitHeaderFuture&& other) noexcept
            : m_connection(rstd::move(other.m_connection)),
              m_state(rstd::move(other.m_state)),
              m_started(other.m_started) {}

        auto operator=(WaitHeaderFuture&& other) noexcept -> WaitHeaderFuture& {
            if (this != &other) {
                cancel();
                m_connection = rstd::move(other.m_connection);
                m_state      = rstd::move(other.m_state);
                m_started    = other.m_started;
            }
            return *this;
        }

        ~WaitHeaderFuture() { cancel(); }

        auto poll(rstd::pin::Pin<rstd::mut_ref<WaitHeaderFuture>> self, rstd::task::Context& cx)
            -> rstd::task::Poll<rstd::Option<Error>> {
            auto& future = *self.get_unchecked_mut();
            if (! future.m_started) {
                future.m_started = true;
                future.m_connection->start_wait_header(future.m_state);
            }
            return future.m_state->poll(cx);
        }

    private:
        void cancel() {
            if (! m_state) return;
            m_state->cancel();
            if (m_connection) {
                m_connection->cancel_wait_header(m_state);
            }
            m_state.reset();
            m_connection.reset();
        }

        Arc<Connection>                                  m_connection;
        rstd_coro::PollStateArc<rstd::Option<Error>>     m_state;
        bool                                             m_started { false };
    };

    auto read_some(rstd::bytes::BytesMut& buffer) -> ReadSomeFuture {
        return ReadSomeFuture { get_arc(), buffer };
    }

    auto write_some(rstd::bytes::Bytes& buffer) -> WriteSomeFuture {
        return WriteSomeFuture { get_arc(), buffer };
    }

    auto wait_header() -> WaitHeaderFuture { return WaitHeaderFuture { get_arc() }; }

private:
    using RstdIoState     = rstd_coro::PollStateArc<IoResult>;
    using RstdHeaderState = rstd_coro::PollStateArc<rstd::Option<Error>>;

    struct RstdReadWaiter {
        rstd::bytes::BytesMut* buffer;
        RstdIoState            state;
    };

    struct RstdWriteWaiter {
        rstd::bytes::Bytes* buffer;
        RstdIoState         state;
    };

    void start_read_some(rstd::bytes::BytesMut& buffer, RstdIoState state) {
        auto lock = std::lock_guard { m_mutex };
        if (state->is_canceled()) return;
        if (m_read_some_future.is_some()) {
            state->set_ready(IoResult::fail(Error::InvalidState("curl read already pending")));
            return;
        }
        m_read_some_future = Some(RstdReadWaiter { &buffer, rstd::move(state) });
        try_read_some_future_locked();
    }

    void start_write_some(rstd::bytes::Bytes& buffer, RstdIoState state) {
        auto lock = std::lock_guard { m_mutex };
        if (state->is_canceled()) return;
        if (m_write_some_future.is_some()) {
            state->set_ready(IoResult::fail(Error::InvalidState("curl write already pending")));
            return;
        }
        m_write_some_future = Some(RstdWriteWaiter { &buffer, rstd::move(state) });
        try_write_some_future_locked();
    }

    void start_wait_header(RstdHeaderState state) {
        auto lock = std::lock_guard { m_mutex };
        if (state->is_canceled()) return;
        if (m_wait_header_future.is_some()) {
            state->set_ready(Some(Error::InvalidState("curl header wait already pending")));
            return;
        }
        m_wait_header_future = Some(rstd::move(state));
        try_wait_header_future_locked();
    }

    void cancel_read_some(const RstdIoState& state) {
        auto lock = std::lock_guard { m_mutex };
        if (m_read_some_future.is_some() && m_read_some_future->state == state) {
            m_read_some_future = None();
        }
    }

    void cancel_write_some(const RstdIoState& state) {
        auto lock = std::lock_guard { m_mutex };
        if (m_write_some_future.is_some() && m_write_some_future->state == state) {
            m_write_some_future = None();
        }
    }

    void cancel_wait_header(const RstdHeaderState& state) {
        auto lock = std::lock_guard { m_mutex };
        if (m_wait_header_future.is_some() && *m_wait_header_future == state) {
            m_wait_header_future = None();
        }
    }

    static usize header_callback(char* ptr, usize size, usize nmemb, Connection* self) {
        std::string_view header { ptr, size * nmemb };
        auto             lock = std::lock_guard { self->m_mutex };

        self->m_header_raw.append(header);
        if (! self->m_header.start) {
            self->m_header.start = HttpHeader::parse_start_line(header);
        } else {
            auto field = HttpHeader::parse_field_line(header);
            if (! field.name.empty()) {
                self->m_header.fields.push_back(field);
                if (helper::starts_with_i(field.name, "set-cookie")) {
                    self->m_cookie_jar.raw_cookie.append(header).push_back('\n');
                }
            }
        }
        if (self->m_header.start && header == "\r\n") {
            self->m_header_done = true;
            self->try_wait_header_future_locked();
        }
        return header.size();
    }

    static usize write_callback(char* ptr, usize size, usize nmemb, Connection* self) {
        auto total_size = size * nmemb;
        auto lock       = std::lock_guard { self->m_mutex };

        if (self->m_recv_buf.is_full()) {
            self->m_recv_paused.store(true);
            return CURL_WRITEFUNC_PAUSE;
        }

        self->try_wait_header_future_locked();
        self->m_recv_buf.commit(reinterpret_cast<const u8*>(ptr), total_size);
        self->try_read_some_future_locked();
        return total_size;
    }

    static usize read_callback(char* ptr, usize size, usize nmemb, Connection* self) {
        auto total_size = size * nmemb;
        if (self->m_send_callback) {
            return self->m_send_callback((byte*)ptr, total_size);
        }

        auto lock = std::lock_guard { self->m_mutex };
        if (self->m_send_buf.empty()) {
            self->m_send_paused.store(true);
            return CURL_READFUNC_PAUSE;
        }

        auto copied = self->m_send_buf.consume(reinterpret_cast<u8*>(ptr), total_size);
        self->try_write_some_future_locked();
        return copied;
    }

    void finish(CURLcode ec) {
        auto lock  = std::lock_guard { m_mutex };
        m_finish_ec = ec;
        m_state     = State::Finished;
        try_read_some_future_locked();
        try_write_some_future_locked();
        try_wait_header_future_locked();
    }

    void cancel() {
        auto lock = std::lock_guard { m_mutex };
        if (m_state != State::Finished && m_state != State::Canceled) {
            m_state = State::Canceled;
        }
        try_read_some_future_locked();
        try_write_some_future_locked();
        try_wait_header_future_locked();
    }

    void transfreing() {
        auto lock = std::lock_guard { m_mutex };
        if (m_state == State::NotStarted) m_state = State::Transfering;
    }

    auto finish_error_locked() const -> rstd::Option<Error> {
        if (m_state == State::Canceled) return Some(Error::Canceled());
        if (m_finish_ec != CURLcode::CURLE_OK) return Some(Error::Curl(m_finish_ec));
        return None<Error>();
    }

    void try_read_some_future_locked() {
        auto waiter_option = m_read_some_future.take();
        if (waiter_option.is_none()) return;

        auto waiter = rstd::move(waiter_option).unwrap_unchecked();
        if (waiter.state->is_canceled()) return;

        auto recv_size = m_recv_buf.size();
        if (m_state == State::Canceled) {
            waiter.state->set_ready(IoResult::fail(Error::Canceled()));
        } else if (recv_size > 0) {
            auto copied = m_recv_buf.consume(*waiter.buffer);
            waiter.state->set_ready(IoResult::ok(copied));
            bool pause { true };
            if (m_recv_buf.size() == 0 && m_recv_paused.compare_exchange_strong(
                                              pause, false, Ordering::SeqCst, Ordering::SeqCst)) {
                send_action(Action::UnPauseRecv);
            }
        } else if (m_state == State::Finished) {
            auto err = finish_error_locked();
            if (err.is_some()) {
                waiter.state->set_ready(IoResult::fail(rstd::move(err).unwrap_unchecked()));
            } else {
                waiter.state->set_ready(IoResult::done());
            }
        } else {
            m_read_some_future = Some(rstd::move(waiter));
        }
    }

    void try_write_some_future_locked() {
        auto waiter_option = m_write_some_future.take();
        if (waiter_option.is_none()) return;

        auto waiter = rstd::move(waiter_option).unwrap_unchecked();
        if (waiter.state->is_canceled()) return;

        if (m_state == State::Canceled) {
            waiter.state->set_ready(IoResult::fail(Error::Canceled()));
        } else if (! m_send_buf.is_full()) {
            auto copied = m_send_buf.commit(*waiter.buffer);
            waiter.state->set_ready(IoResult::ok(copied));
            bool pause { true };
            if (m_send_paused.compare_exchange_strong(
                    pause, false, Ordering::SeqCst, Ordering::SeqCst)) {
                send_action(Action::UnPauseSend);
            }
        } else if (m_state == State::Finished) {
            auto err = finish_error_locked();
            if (err.is_some()) {
                waiter.state->set_ready(IoResult::fail(rstd::move(err).unwrap_unchecked()));
            } else {
                waiter.state->set_ready(IoResult::done());
            }
        } else {
            m_write_some_future = Some(rstd::move(waiter));
        }
    }

    void try_wait_header_future_locked() {
        if (m_wait_header_future.is_none()) return;
        if (! m_header_done && m_state != State::Canceled && m_state != State::Finished) {
            return;
        }

        auto state           = rstd::move(m_wait_header_future).unwrap_unchecked();
        m_wait_header_future = None();
        if (state->is_canceled()) return;

        state->set_ready(finish_error_locked());
    }

    std::string m_url;

    CURLcode m_finish_ec;
    State    m_state;
    Atomic<bool> m_recv_paused;
    Atomic<bool> m_send_paused;

    Box<CurlEasy>       m_easy;
    Arc<SessionChannel> m_session_channel;

    std::string m_header_raw;
    HttpHeader   m_header;
    bool         m_header_done { false };
    CookieJar    m_cookie_jar;

    Buffer<allocator_type> m_recv_buf;

    req_opt::Read::Callback m_send_callback;
    Buffer<allocator_type>  m_send_buf;

    Option<RstdHeaderState> m_wait_header_future;
    Option<RstdReadWaiter>  m_read_some_future;
    Option<RstdWriteWaiter> m_write_some_future;

    mutable std::mutex m_mutex;
};

} // namespace ncrequest::client::curl
