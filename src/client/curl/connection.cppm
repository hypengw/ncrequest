module;
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

template<typename T>
struct CompletionProducer {
    rstd::async::CompletionHandle<T> handle;

    explicit CompletionProducer(rstd::async::CompletionHandle<T> handle)
        : handle(rstd::move(handle)) {}

    void complete(T value) { (void)handle.complete(rstd::move(value)); }
    auto is_closed() -> bool { return handle.is_closed(); }
};

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
        static auto fail(Error error) -> IoResult { return { Some(rstd::move(error)), false, 0 }; }
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

    auto& header() const { return *m_header; }
    auto trailers() const -> Option<ref<http::Header>> {
        if (m_trailers.is_none()) return None<ref<http::Header>>();
        return Some(ref<http::Header>::from_raw_parts(&*m_trailers));
    }
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

    auto read_some(rstd::bytes::BytesMut& buffer) -> coro<IoResult> {
        auto made = rstd::async::Completion<IoResult>::make();
        if (made.is_err()) {
            co_return IoResult::fail(Error::Io(rstd::move(made).unwrap_err_unchecked()));
        }
        auto pair     = rstd::move(made).unwrap_unchecked();
        auto receiver = rstd::move(pair.get<0>());
        auto state    = make_arc<CompletionProducer<IoResult>>(rstd::move(pair.get<1>()));

        struct CancelOnDrop {
            Arc<Connection>                   connection;
            Arc<CompletionProducer<IoResult>> state;
            ~CancelOnDrop() { connection->cancel_read_some(state); }
        };
        auto cancel = CancelOnDrop { get_arc(), state };
        start_read_some(buffer, state);
        auto result = co_await rstd::move(receiver);
        if (result.is_err()) {
            co_return IoResult::fail(Error::Canceled());
        }
        co_return rstd::move(result).unwrap_unchecked();
    }

    auto write_some(rstd::bytes::Bytes& buffer) -> coro<IoResult> {
        auto made = rstd::async::Completion<IoResult>::make();
        if (made.is_err()) {
            co_return IoResult::fail(Error::Io(rstd::move(made).unwrap_err_unchecked()));
        }
        auto pair     = rstd::move(made).unwrap_unchecked();
        auto receiver = rstd::move(pair.get<0>());
        auto state    = make_arc<CompletionProducer<IoResult>>(rstd::move(pair.get<1>()));

        struct CancelOnDrop {
            Arc<Connection>                   connection;
            Arc<CompletionProducer<IoResult>> state;
            ~CancelOnDrop() { connection->cancel_write_some(state); }
        };
        auto cancel = CancelOnDrop { get_arc(), state };
        start_write_some(buffer, state);
        auto result = co_await rstd::move(receiver);
        if (result.is_err()) {
            co_return IoResult::fail(Error::Canceled());
        }
        co_return rstd::move(result).unwrap_unchecked();
    }

    auto wait_header() -> coro<rstd::Option<Error>> {
        using Output = rstd::Option<Error>;
        auto made    = rstd::async::Completion<Output>::make();
        if (made.is_err()) {
            co_return Some(Error::Io(rstd::move(made).unwrap_err_unchecked()));
        }
        auto pair     = rstd::move(made).unwrap_unchecked();
        auto receiver = rstd::move(pair.get<0>());
        auto state    = make_arc<CompletionProducer<Output>>(rstd::move(pair.get<1>()));

        struct CancelOnDrop {
            Arc<Connection>                 connection;
            Arc<CompletionProducer<Output>> state;
            ~CancelOnDrop() { connection->cancel_wait_header(state); }
        };
        auto cancel = CancelOnDrop { get_arc(), state };
        start_wait_header(state);
        auto result = co_await rstd::move(receiver);
        if (result.is_err()) {
            co_return Some(Error::Canceled());
        }
        co_return rstd::move(result).unwrap_unchecked();
    }

private:
    using RstdIoState     = Arc<CompletionProducer<IoResult>>;
    using RstdHeaderState = Arc<CompletionProducer<rstd::Option<Error>>>;

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
        if (state->is_closed()) return;
        if (m_read_waiter.is_some()) {
            state->complete(IoResult::fail(Error::InvalidState("curl read already pending")));
            return;
        }
        m_read_waiter = Some(RstdReadWaiter { &buffer, rstd::move(state) });
        try_read_waiter_locked();
    }

    void start_write_some(rstd::bytes::Bytes& buffer, RstdIoState state) {
        auto lock = std::lock_guard { m_mutex };
        if (state->is_closed()) return;
        if (m_write_waiter.is_some()) {
            state->complete(IoResult::fail(Error::InvalidState("curl write already pending")));
            return;
        }
        m_write_waiter = Some(RstdWriteWaiter { &buffer, rstd::move(state) });
        try_write_waiter_locked();
    }

    void start_wait_header(RstdHeaderState state) {
        auto lock = std::lock_guard { m_mutex };
        if (state->is_closed()) return;
        if (m_header_waiter.is_some()) {
            state->complete(Some(Error::InvalidState("curl header wait already pending")));
            return;
        }
        m_header_waiter = Some(rstd::move(state));
        try_header_waiter_locked();
    }

    void cancel_read_some(const RstdIoState& state) {
        auto lock = std::lock_guard { m_mutex };
        if (m_read_waiter.is_some() && m_read_waiter->state == state) {
            m_read_waiter = None();
        }
    }

    void cancel_write_some(const RstdIoState& state) {
        auto lock = std::lock_guard { m_mutex };
        if (m_write_waiter.is_some() && m_write_waiter->state == state) {
            m_write_waiter = None();
        }
    }

    void cancel_wait_header(const RstdHeaderState& state) {
        auto lock = std::lock_guard { m_mutex };
        if (m_header_waiter.is_some() && *m_header_waiter == state) {
            m_header_waiter = None();
        }
    }

    static usize header_callback(char* ptr, usize size, usize nmemb, Connection* self) {
        std::string_view header { ptr, size * nmemb };
        auto             lock = std::lock_guard { self->m_mutex };

        if (self->m_body_started) {
            self->m_trailer_started = true;
            auto parsed = self->m_trailer_parser.push(rstd::slice<rstd::u8>::from_raw_parts(
                reinterpret_cast<const rstd::u8*>(header.data()), header.size()));
            if (parsed.is_err()) {
                self->m_header_error = Some(rstd::move(parsed).unwrap_err());
                return 0;
            }
            auto event = rstd::move(parsed).unwrap();
            if (event.is_Complete()) {
                self->m_trailers = Some(rstd::move(event).as_Complete().fields);
            }
            return header.size();
        }
        if (self->m_header_done) self->m_header_done = false;

        auto parsed = self->m_header_parser.push(rstd::slice<rstd::u8>::from_raw_parts(
            reinterpret_cast<const rstd::u8*>(header.data()), header.size()));
        if (parsed.is_err()) {
            self->m_header_error = Some(rstd::move(parsed).unwrap_err());
            self->m_header_done  = true;
            self->try_header_waiter_locked();
            return 0;
        }

        auto event = rstd::move(parsed).unwrap();
        if (event.is_Complete()) {
            auto completed = rstd::move(event).as_Complete();
            self->m_header = Some(rstd::move(completed.head));
            self->m_header_done = true;

            self->m_header_parser = http::Http1HeadParser {};
        }
        return header.size();
    }

    static usize write_callback(char* ptr, usize size, usize nmemb, Connection* self) {
        auto total_size = size * nmemb;
        auto lock       = std::lock_guard { self->m_mutex };

        self->m_body_started = true;
        self->try_header_waiter_locked();
        if (self->m_recv_buf.is_full()) {
            self->m_recv_paused.store(true);
            return CURL_WRITEFUNC_PAUSE;
        }

        self->try_header_waiter_locked();
        self->m_recv_buf.commit(reinterpret_cast<const u8*>(ptr), total_size);
        self->try_read_waiter_locked();
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
        self->try_write_waiter_locked();
        return copied;
    }

    void finish(CURLcode ec) {
        auto lock   = std::lock_guard { m_mutex };
        if (m_trailer_started && m_trailers.is_none() && m_header_error.is_none()) {
            auto parsed = m_trailer_parser.push(rstd::str_::as_bytes("\r\n"));
            if (parsed.is_err()) {
                m_header_error = Some(rstd::move(parsed).unwrap_err());
            } else {
                auto event = rstd::move(parsed).unwrap();
                if (event.is_Complete()) {
                    m_trailers = Some(rstd::move(event).as_Complete().fields);
                } else {
                    auto incomplete = m_trailer_parser.finish();
                    m_header_error = Some(rstd::move(incomplete).unwrap_err());
                }
            }
        }
        m_finish_ec = ec;
        m_state     = State::Finished;
        try_read_waiter_locked();
        try_write_waiter_locked();
        try_header_waiter_locked();
    }

    void cancel() {
        auto lock = std::lock_guard { m_mutex };
        if (m_state != State::Finished && m_state != State::Canceled) {
            m_state = State::Canceled;
        }
        try_read_waiter_locked();
        try_write_waiter_locked();
        try_header_waiter_locked();
    }

    void transfreing() {
        auto lock = std::lock_guard { m_mutex };
        if (m_state == State::NotStarted) m_state = State::Transfering;
    }

    auto finish_error_locked() const -> rstd::Option<Error> {
        if (m_header_error.is_some()) {
            auto const& kind = m_header_error->kind();
            auto protocol = ProtocolError::InvalidHeaderLine;
            if (kind.is_InvalidStartLine()) {
                protocol = ProtocolError::InvalidStatusLine;
            } else if (kind.is_HeaderTooLarge()) {
                protocol = ProtocolError::HeaderTooLarge;
            } else if (kind.is_UnexpectedEof()) {
                protocol = ProtocolError::UnexpectedEof;
            }
            return Some(Error::Protocol(protocol, protocol_error_message(protocol)));
        }
        if (m_state == State::Canceled) return Some(Error::Canceled());
        if (m_finish_ec != CURLcode::CURLE_OK) {
            return Some(rstd::into<Error>(static_cast<CURLcode>(m_finish_ec)));
        }
        if (m_state == State::Finished && ! m_header_done) {
            auto protocol = ProtocolError::UnexpectedEof;
            return Some(Error::Protocol(protocol, protocol_error_message(protocol)));
        }
        return None<Error>();
    }

    void try_read_waiter_locked() {
        auto waiter_option = m_read_waiter.take();
        if (waiter_option.is_none()) return;

        auto waiter = rstd::move(waiter_option).unwrap_unchecked();
        if (waiter.state->is_closed()) return;

        auto recv_size = m_recv_buf.size();
        if (m_state == State::Canceled) {
            waiter.state->complete(IoResult::fail(Error::Canceled()));
        } else if (recv_size > 0) {
            auto copied = m_recv_buf.consume(*waiter.buffer);
            waiter.state->complete(IoResult::ok(copied));
            bool pause { true };
            if (m_recv_buf.size() == 0 && m_recv_paused.compare_exchange_strong(
                                              pause, false, Ordering::SeqCst, Ordering::SeqCst)) {
                send_action(Action::UnPauseRecv);
            }
        } else if (m_state == State::Finished) {
            auto err = finish_error_locked();
            if (err.is_some()) {
                waiter.state->complete(IoResult::fail(rstd::move(err).unwrap_unchecked()));
            } else {
                waiter.state->complete(IoResult::done());
            }
        } else {
            m_read_waiter = Some(rstd::move(waiter));
        }
    }

    void try_write_waiter_locked() {
        auto waiter_option = m_write_waiter.take();
        if (waiter_option.is_none()) return;

        auto waiter = rstd::move(waiter_option).unwrap_unchecked();
        if (waiter.state->is_closed()) return;

        if (m_state == State::Canceled) {
            waiter.state->complete(IoResult::fail(Error::Canceled()));
        } else if (! m_send_buf.is_full()) {
            auto copied = m_send_buf.commit(*waiter.buffer);
            waiter.state->complete(IoResult::ok(copied));
            bool pause { true };
            if (m_send_paused.compare_exchange_strong(
                    pause, false, Ordering::SeqCst, Ordering::SeqCst)) {
                send_action(Action::UnPauseSend);
            }
        } else if (m_state == State::Finished) {
            auto err = finish_error_locked();
            if (err.is_some()) {
                waiter.state->complete(IoResult::fail(rstd::move(err).unwrap_unchecked()));
            } else {
                waiter.state->complete(IoResult::done());
            }
        } else {
            m_write_waiter = Some(rstd::move(waiter));
        }
    }

    void try_header_waiter_locked() {
        if (m_header_waiter.is_none()) return;
        if (m_state != State::Canceled && m_state != State::Finished &&
            (! m_header_done || ! m_body_started)) {
            return;
        }

        auto state      = rstd::move(m_header_waiter).unwrap_unchecked();
        m_header_waiter = None();
        if (state->is_closed()) return;

        state->complete(finish_error_locked());
    }

    std::string m_url;

    CURLcode     m_finish_ec;
    State        m_state;
    Atomic<bool> m_recv_paused;
    Atomic<bool> m_send_paused;

    Box<CurlEasy>       m_easy;
    Arc<SessionChannel> m_session_channel;

    http::Http1HeadParser        m_header_parser;
    http::Http1FieldSectionParser m_trailer_parser;
    Option<http::MessageHead>    m_header;
    Option<http::Header>         m_trailers;
    Option<http::HttpParseError> m_header_error;
    bool                         m_header_done { false };
    bool                         m_body_started { false };
    bool                         m_trailer_started { false };
    Buffer<allocator_type> m_recv_buf;

    req_opt::Read::Callback m_send_callback;
    Buffer<allocator_type>  m_send_buf;

    Option<RstdHeaderState> m_header_waiter;
    Option<RstdReadWaiter>  m_read_waiter;
    Option<RstdWriteWaiter> m_write_waiter;

    mutable std::mutex m_mutex;
};

} // namespace ncrequest::client::curl
