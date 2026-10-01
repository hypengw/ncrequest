module;
#include <rstd/enum.hpp>

export module ncrequest:client_curl_connection;
export import rstd;
export import ncrequest.curl;
export import ncrequest.coro;
export import lihttpto;
export import :request;
export import :error;
export import :client_callback;

using namespace rstd::prelude;
using namespace ::curl;
using namespace rstd::literals;
using rstd::async::Completion;
using rstd::async::CompletionHandle;
using rstd::bytes::Bytes;
using rstd::bytes::BytesMut;
using rstd::sync::Arc;
using rstd::sync::Condvar;
using rstd::sync::Mutex;
using rstd::sync::MutexGuard;
using rstd::sync::Weak;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

namespace ncrequest::client::curl
{

class RawMutexGuard {
    MutexGuard<empty> m_guard;

public:
    explicit RawMutexGuard(Mutex<empty> const& mutex): m_guard(mutex.lock().unwrap()) {}

    RawMutexGuard(const RawMutexGuard&)                    = delete;
    auto operator=(const RawMutexGuard&) -> RawMutexGuard& = delete;
    RawMutexGuard(RawMutexGuard&&)                         = delete;
    auto operator=(RawMutexGuard&&) -> RawMutexGuard&      = delete;
};

export class SessionBackend;

template<typename T>
struct CompletionProducer {
    CompletionHandle<T> handle;

    explicit CompletionProducer(CompletionHandle<T> handle): handle(rstd::move(handle)) {}

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

class Message final {
    RSTD_ENUM_DEFAULT(Message, (Stop), (Stop),
                      (ConnectAction, (Arc<Connection> con; Action action;)))
};
} // namespace session_message

export using SessionMessage = session_message::Message;

export class SessionChannel {
public:
    SessionChannel(const SessionChannel&)                    = delete;
    auto operator=(const SessionChannel&) -> SessionChannel& = delete;

    using WakeCallback = client::Callback<void()>;

    SessionChannel(): m_fields(Fields {}) {}

    void set_wake_callback(WakeCallback callback) {
        auto fields  = m_fields.lock().unwrap();
        fields->wake = rstd::move(callback);
    }

    auto try_send(SessionMessage msg) -> bool {
        WakeCallback wake;
        {
            auto fields = m_fields.lock().unwrap();
            if (fields->closed) return false;
            if (msg.is_Stop()) fields->closed = true;
            fields->messages.push(rstd::move(msg));
            wake = fields->wake.clone();
        }
        m_cv.notify_one();
        if (wake) wake();
        return true;
    }

    auto try_receive(SessionMessage& out) -> bool {
        auto fields = m_fields.lock().unwrap();
        if (fields->messages.is_empty()) return false;

        out = fields->messages.remove(usize());
        return true;
    }

    auto receive() -> SessionMessage {
        auto fields = m_fields.lock().unwrap();
        m_cv.wait_while(fields, [](const Fields& value) {
            return value.messages.is_empty();
        });
        return fields->messages.remove(usize());
    }

private:
    struct Fields {
        Vec<SessionMessage> messages;
        WakeCallback        wake;
        bool                closed { false };

        Fields(): messages(Vec<SessionMessage>::make()) {}
    };

    Mutex<Fields> m_fields;
    Condvar       m_cv;
};

export class Connection {
    friend class SessionBackend;

public:
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
        Option<Error> error;
        bool          eof { false };
        usize         size { 0 };

        static auto ok(usize size) -> IoResult { return { None<Error>(), false, size }; }
        static auto done() -> IoResult { return { None<Error>(), true, usize() }; }
        static auto fail(Error error) -> IoResult {
            return { Some(rstd::move(error)), false, usize() };
        }
    };

    class Buffer {
    public:
        explicit Buffer(usize limit): m_state(State::Empty), m_limit(limit), m_transferred() {}

        enum class State : rstd::int32_t
        {
            Empty = 0,
            Normal,
            Full,
        };

        bool is_full() const { return m_state.load() == State::Full; }
        bool empty() const { return m_state.load() == State::Empty; }

        auto size() const { return m_buf.size(); }
        auto available() const { return m_limit - m_buf.size(); }
        auto data() const { return m_buf.data(); }

        auto commit(slice<u8> in) {
            auto copied = rstd::min(in.len(), available());
            m_buf.put_slice(slice<u8>::from_raw_parts(in.as_raw_ptr(), copied));
            m_transferred += copied;
            check_full();
            return copied;
        }

        auto consume(BytesMut& out) {
            auto chunk  = out.chunk_mut();
            auto copied = rstd::min(chunk.len(), m_buf.size());
            if (copied == usize()) return usize();

            rstd::mem::memcpy(chunk.as_raw_ptr(), m_buf.data(), copied);
            out.advance_mut(copied);
            m_buf.advance(copied);
            check_full();
            return copied;
        }

        auto consume(mut_ref<u8[]> out) {
            auto copied = rstd::min(out.len(), m_buf.size());
            if (copied == usize()) return usize();

            rstd::mem::memcpy(out.as_raw_ptr(), m_buf.as_slice().as_raw_ptr(), copied);
            m_buf.advance(copied);
            check_full();
            return copied;
        }

        auto commit(Bytes& in) {
            auto chunk  = in.chunk();
            auto copied = commit(chunk);
            in.advance(copied);
            return copied;
        }

    private:
        void check_full() {
            auto s = size();
            m_state.store(s == usize() ? State::Empty
                                       : (s >= m_limit ? State::Full : State::Normal));
        }

        BytesMut      m_buf;
        Atomic<State> m_state;
        usize         m_limit;
        usize         m_transferred;
    };

    static auto make(PreparedRequest request, Arc<SessionChannel> session_channel)
        -> Arc<Connection> {
        auto connection = Arc<Connection>::make(rstd::move(request), rstd::move(session_channel));
        connection->m_self = connection.downgrade();
        return connection;
    }

    Connection(PreparedRequest request, Arc<SessionChannel> session_channel)
        : m_finish_ec(CURLcode::CURLE_OK),
          m_state(State::NotStarted),
          m_recv_paused(false),
          m_send_paused(false),
          m_request(rstd::move(request)),
          m_easy(Box<CurlEasy>::make()),
          m_session_channel(rstd::move(session_channel)),
          m_recv_buf(RECV_LIMIT),
          m_send_buf(SEND_LIMIT),
          m_mutex(empty {}),
          m_self(Weak<Connection>::make()) {
        auto& easy = *m_easy;
        easy.setopt(CURLoption::CURLOPT_WRITEFUNCTION, Connection::write_callback);
        easy.setopt(CURLoption::CURLOPT_WRITEDATA, this);

        easy.setopt(CURLoption::CURLOPT_HEADERFUNCTION, Connection::header_callback);
        easy.setopt(CURLoption::CURLOPT_HEADERDATA, this);

        easy.setopt(CURLoption::CURLOPT_READFUNCTION, Connection::read_callback);
        easy.setopt(CURLoption::CURLOPT_READDATA, this);
        easy.setopt(CURLoption::CURLOPT_PRIVATE, this);
    }

    auto get_arc() -> Arc<Connection> {
        auto self = m_self.upgrade();
        if (! self) rstd::panic { "Connection is not bound to its Arc owner" };
        return self;
    }

    auto& easy() { return *m_easy; }
    auto  request() const -> const Request& { return m_request.request(); }
    auto  options() const -> const EffectiveOptions& { return m_request.options(); }
    auto& easy() const { return *m_easy; }
    auto& channel() { return m_session_channel; }

    auto& header() const { return *m_header; }
    auto  trailers() const -> Option<ref<lihttpto::Headers>> {
        auto lock = RawMutexGuard { m_mutex };
        if (m_state != State::Finished || m_trailers.is_none())
            return None<ref<lihttpto::Headers>>();
        return Some(ref<lihttpto::Headers>::from_raw_parts(&*m_trailers));
    }
    void set_send_callback(BodyReader::Callback cb) { m_send_callback = rstd::move(cb); }

    auto is_finished() const -> bool {
        auto lock = RawMutexGuard { m_mutex };
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
            auto lock = RawMutexGuard { m_mutex };
            state     = m_state;
        }
        if (state == State::Canceled || state == State::Finished) return;

        auto msg = SessionMessage::ConnectAction(get_arc(), session_message::Action::Cancel);
        m_session_channel->try_send(rstd::move(msg));
    }

    auto read_some(BytesMut& buffer) -> coro<IoResult> {
        auto made = Completion<IoResult>::make();
        if (made.is_err()) {
            co_return IoResult::fail(Error::Io(rstd::move(made).unwrap_err_unchecked()));
        }
        auto pair     = rstd::move(made).unwrap_unchecked();
        auto receiver = rstd::move(pair.get<0>());
        auto state    = Arc<CompletionProducer<IoResult>>::make(rstd::move(pair.get<1>()));

        struct CancelOnDrop {
            Arc<Connection>                   connection;
            Arc<CompletionProducer<IoResult>> state;
            ~CancelOnDrop() { connection->cancel_read_some(state); }
        };
        auto cancel = CancelOnDrop { get_arc(), state.clone() };
        start_read_some(buffer, rstd::move(state));
        auto result = co_await rstd::move(receiver);
        if (result.is_err()) {
            co_return IoResult::fail(Error::Canceled());
        }
        co_return rstd::move(result).unwrap_unchecked();
    }

    auto write_some(Bytes& buffer) -> coro<IoResult> {
        auto made = Completion<IoResult>::make();
        if (made.is_err()) {
            co_return IoResult::fail(Error::Io(rstd::move(made).unwrap_err_unchecked()));
        }
        auto pair     = rstd::move(made).unwrap_unchecked();
        auto receiver = rstd::move(pair.get<0>());
        auto state    = Arc<CompletionProducer<IoResult>>::make(rstd::move(pair.get<1>()));

        struct CancelOnDrop {
            Arc<Connection>                   connection;
            Arc<CompletionProducer<IoResult>> state;
            ~CancelOnDrop() { connection->cancel_write_some(state); }
        };
        auto cancel = CancelOnDrop { get_arc(), state.clone() };
        start_write_some(buffer, rstd::move(state));
        auto result = co_await rstd::move(receiver);
        if (result.is_err()) {
            co_return IoResult::fail(Error::Canceled());
        }
        co_return rstd::move(result).unwrap_unchecked();
    }

    auto wait_header() -> coro<Option<Error>> {
        using Output = Option<Error>;
        auto made    = Completion<Output>::make();
        if (made.is_err()) {
            co_return Some(Error::Io(rstd::move(made).unwrap_err_unchecked()));
        }
        auto pair     = rstd::move(made).unwrap_unchecked();
        auto receiver = rstd::move(pair.get<0>());
        auto state    = Arc<CompletionProducer<Output>>::make(rstd::move(pair.get<1>()));

        struct CancelOnDrop {
            Arc<Connection>                 connection;
            Arc<CompletionProducer<Output>> state;
            ~CancelOnDrop() { connection->cancel_wait_header(state); }
        };
        auto cancel = CancelOnDrop { get_arc(), state.clone() };
        start_wait_header(rstd::move(state));
        auto result = co_await rstd::move(receiver);
        if (result.is_err()) {
            co_return Some(Error::Canceled());
        }
        co_return rstd::move(result).unwrap_unchecked();
    }

private:
    using RstdIoState     = Arc<CompletionProducer<IoResult>>;
    using RstdHeaderState = Arc<CompletionProducer<Option<Error>>>;

    struct RstdReadWaiter {
        BytesMut*   buffer;
        RstdIoState state;
    };

    struct RstdWriteWaiter {
        Bytes*      buffer;
        RstdIoState state;
    };

    void start_read_some(BytesMut& buffer, RstdIoState state) {
        auto lock = RawMutexGuard { m_mutex };
        if (state->is_closed()) return;
        if (m_read_waiter.is_some()) {
            state->complete(IoResult::fail(Error::InvalidState("curl read already pending")));
            return;
        }
        m_read_waiter = Some(RstdReadWaiter { &buffer, rstd::move(state) });
        try_read_waiter_locked();
    }

    void start_write_some(Bytes& buffer, RstdIoState state) {
        auto lock = RawMutexGuard { m_mutex };
        if (state->is_closed()) return;
        if (m_write_waiter.is_some()) {
            state->complete(IoResult::fail(Error::InvalidState("curl write already pending")));
            return;
        }
        m_write_waiter = Some(RstdWriteWaiter { &buffer, rstd::move(state) });
        try_write_waiter_locked();
    }

    void start_wait_header(RstdHeaderState state) {
        auto lock = RawMutexGuard { m_mutex };
        if (state->is_closed()) return;
        if (m_header_waiter.is_some()) {
            state->complete(Some(Error::InvalidState("curl header wait already pending")));
            return;
        }
        m_header_waiter = Some(rstd::move(state));
        try_header_waiter_locked();
    }

    void cancel_read_some(const RstdIoState& state) {
        auto lock = RawMutexGuard { m_mutex };
        if (m_read_waiter.is_some() && RstdIoState::ptr_eq(m_read_waiter->state, state)) {
            m_read_waiter = None();
        }
    }

    void cancel_write_some(const RstdIoState& state) {
        auto lock = RawMutexGuard { m_mutex };
        if (m_write_waiter.is_some() && RstdIoState::ptr_eq(m_write_waiter->state, state)) {
            m_write_waiter = None();
        }
    }

    void cancel_wait_header(const RstdHeaderState& state) {
        auto lock = RawMutexGuard { m_mutex };
        if (m_header_waiter.is_some() && RstdHeaderState::ptr_eq(*m_header_waiter, state)) {
            m_header_waiter = None();
        }
    }

    static rstd::size_t header_callback(char* ptr, rstd::size_t size, rstd::size_t nmemb,
                                        Connection* self) {
        auto total_size = usize(size * nmemb);
        auto header     = slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(ptr), total_size);
        auto lock       = RawMutexGuard { self->m_mutex };

        if (self->m_header_done) {
            self->m_trailer_started = true;
            auto parsed             = self->m_trailer_parser.push(header);
            if (parsed.is_err()) {
                self->m_header_error = Some(rstd::move(parsed).unwrap_err());
                return 0;
            }
            auto event = rstd::move(parsed).unwrap();
            if (event.is_Complete()) {
                self->m_trailers = Some(rstd::move(event).as_Complete().fields);
            }
            return header.len().to_primitive();
        }

        auto parsed = self->m_header_parser.push(header);
        if (parsed.is_err()) {
            self->m_header_error = Some(rstd::move(parsed).unwrap_err());
            self->m_header_done  = true;
            self->try_header_waiter_locked();
            return 0;
        }

        auto event = rstd::move(parsed).unwrap();
        if (event.is_Complete()) {
            auto completed = rstd::move(event).as_Complete();
            auto status    = completed.head.status_code();
            if (status.is_none()) return 0;
            auto code     = status->to_primitive();
            auto location = completed.head.headers().get("location"_str);
            auto redirect = code >= 300 && code < 400 && location.is_some() &&
                            (*location)->as_slice().len() != usize();
            if (code >= 200 && ! redirect) {
                self->m_header      = Some(rstd::move(completed.head));
                self->m_header_done = true;
                self->try_header_waiter_locked();
            }
            self->m_header_parser = lihttpto::Http1HeadParser { true };
        }
        return header.len().to_primitive();
    }

    static rstd::size_t write_callback(char* ptr, rstd::size_t size, rstd::size_t nmemb,
                                       Connection* self) {
        auto total_size = usize(size * nmemb);
        auto lock       = RawMutexGuard { self->m_mutex };

        if (total_size > self->m_recv_buf.available()) {
            self->m_recv_paused.store(true);
            return static_cast<rstd::size_t>(CURL_WRITEFUNC_PAUSE);
        }

        self->m_recv_buf.commit(
            slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(ptr), total_size));
        self->try_read_waiter_locked();
        return total_size.to_primitive();
    }

    static rstd::size_t read_callback(char* ptr, rstd::size_t size, rstd::size_t nmemb,
                                      Connection* self) {
        auto total_size = usize(size * nmemb);
        if (self->m_send_callback) {
            return self->m_send_callback(reinterpret_cast<byte*>(ptr), total_size).to_primitive();
        }

        auto lock = RawMutexGuard { self->m_mutex };
        if (self->m_send_buf.empty()) {
            self->m_send_paused.store(true);
            return static_cast<rstd::size_t>(CURL_READFUNC_PAUSE);
        }

        auto output = mut_ref<u8[]>::from_raw_parts(reinterpret_cast<byte*>(ptr), total_size);
        auto copied = self->m_send_buf.consume(output);
        self->try_write_waiter_locked();
        return copied.to_primitive();
    }

    void finish(CURLcode ec) {
        auto lock = RawMutexGuard { m_mutex };
        if (m_state == State::Finished || m_state == State::Canceled) return;
        if (m_trailer_started && m_trailers.is_none() && m_header_error.is_none()) {
            auto parsed = m_trailer_parser.push("\r\n"_bytes);
            if (parsed.is_err()) {
                m_header_error = Some(rstd::move(parsed).unwrap_err());
            } else {
                auto event = rstd::move(parsed).unwrap();
                if (event.is_Complete()) {
                    m_trailers = Some(rstd::move(event).as_Complete().fields);
                } else {
                    auto incomplete = m_trailer_parser.finish();
                    m_header_error  = Some(rstd::move(incomplete).unwrap_err());
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
        auto lock = RawMutexGuard { m_mutex };
        if (m_state != State::Finished && m_state != State::Canceled) {
            m_state = State::Canceled;
        }
        try_read_waiter_locked();
        try_write_waiter_locked();
        try_header_waiter_locked();
    }

    void transfreing() {
        auto lock = RawMutexGuard { m_mutex };
        if (m_state == State::NotStarted) m_state = State::Transfering;
    }

    auto finish_error_locked() const -> Option<Error> {
        if (m_header_error.is_some()) {
            auto const& kind     = m_header_error->kind();
            auto        protocol = ProtocolError::InvalidHeaderLine;
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
        } else if (recv_size > usize()) {
            auto copied = m_recv_buf.consume(*waiter.buffer);
            waiter.state->complete(IoResult::ok(copied));
            bool pause { true };
            if (m_recv_buf.size() == usize() &&
                m_recv_paused.compare_exchange_strong(
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
        if (m_state != State::Canceled && m_state != State::Finished && ! m_header_done) {
            return;
        }

        auto state      = rstd::move(m_header_waiter).unwrap_unchecked();
        m_header_waiter = None();
        if (state->is_closed()) return;

        state->complete(finish_error_locked());
    }

    CURLcode     m_finish_ec;
    State        m_state;
    Atomic<bool> m_recv_paused;
    Atomic<bool> m_send_paused;

    // Keep upload bytes and callbacks alive until easy cleanup, including queued cancellation.
    PreparedRequest     m_request;
    Box<CurlEasy>       m_easy;
    Arc<SessionChannel> m_session_channel;

    lihttpto::Http1HeadParser         m_header_parser { true };
    lihttpto::Http1FieldSectionParser m_trailer_parser;
    Option<lihttpto::MessageHead>     m_header;
    Option<lihttpto::Headers>         m_trailers;
    Option<lihttpto::HttpParseError>  m_header_error;
    bool                              m_header_done { false };
    bool                              m_trailer_started { false };
    Buffer                            m_recv_buf;

    BodyReader::Callback m_send_callback;
    Buffer               m_send_buf;

    Option<RstdHeaderState> m_header_waiter;
    Option<RstdReadWaiter>  m_read_waiter;
    Option<RstdWriteWaiter> m_write_waiter;

    mutable Mutex<empty> m_mutex;
    Weak<Connection>     m_self;
};

} // namespace ncrequest::client::curl
