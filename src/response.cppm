export module ncrequest:response;

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
import :client_qt_network;
#else
import :client_curl_response;
#endif
export import :client_http_backend;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
using rstd::bytes::BytesMut;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;

namespace ncrequest
{

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
using SelectedResponseBackend = client::qt_network::ResponseBackend;
#else
using SelectedResponseBackend = client::curl::ResponseBackend;
#endif

static_assert(client::HttpResponseBackend<SelectedResponseBackend>);

export class Response;
export class Session;

export class ResponseBody {
    friend class Response;
    struct State {
        Arc<SelectedResponseBackend> backend;
        Atomic<bool>                 busy { false };
        usize                        collect_limit;
        bool                         started { false };
        bool                         terminal { false };
        explicit State(Arc<SelectedResponseBackend> value, usize limit)
            : backend(rstd::move(value)), collect_limit(limit) {}
    };
    struct ReadGuard {
        Arc<State> state;
        bool       completed { false };
        ~ReadGuard() {
            if (! completed) {
                state->terminal = true;
                state->backend->cancel();
            }
            state->busy.store(false);
        }
    };
    Arc<State> state_;

    explicit ResponseBody(Arc<SelectedResponseBackend> backend, usize limit)
        : state_(Arc<State>::make(rstd::move(backend), limit)) {}

    static auto next_chunk(State& state) -> coro<Result<Option<Bytes>>> {
        auto result = co_await state.backend->next_chunk();
        if (result.is_err() || result->is_none()) state.terminal = true;
        co_return result;
    }

    static auto read(Arc<State> state) -> coro<Result<Option<Bytes>>> {
        if (! state) co_return Err(Error::InvalidState("response body was moved"));
        if (state->busy.exchange(true))
            co_return Err(Error::InvalidState("response body already has a reader"));
        auto guard      = ReadGuard { state.clone() };
        guard.completed = true;
        if (state->terminal) co_return Err(Error::InvalidState("response body was consumed"));
        state->started  = true;
        guard.completed = false;
        auto result     = co_await next_chunk(*state);
        guard.completed = result.is_ok();
        co_return result;
    }

    static auto collect_body(Arc<State> state, usize limit) -> coro<Result<Bytes>> {
        if (! state) co_return Err(Error::InvalidState("response body was moved"));
        if (state->busy.exchange(true))
            co_return Err(Error::InvalidState("response body already has a reader"));
        auto guard      = ReadGuard { state.clone() };
        guard.completed = true;
        if (state->started || state->terminal)
            co_return Err(Error::InvalidState("response body was consumed"));
        state->started  = true;
        guard.completed = false;
        // A per-call limit may tighten, but never bypass, the resolved request policy.
        limit = rstd::min(limit, state->collect_limit);
        BytesMut out;
        for (;;) {
            auto part = co_await next_chunk(*state);
            if (part.is_err()) co_return Err(rstd::move(part).unwrap_err());
            if (part->is_none()) {
                state->terminal = true;
                guard.completed = true;
                co_return Ok(out.freeze());
            }
            auto bytes = part->take().unwrap();
            if (bytes.size() > limit - out.size())
                co_return Err(Error::Protocol(ProtocolError::BodyTooLarge, nullptr));
            out.extend_from_slice(bytes.as_slice());
        }
    }

public:
    ResponseBody(const ResponseBody&)                    = delete;
    auto operator=(const ResponseBody&) -> ResponseBody& = delete;

    using Error                                = ncrequest::Error;
    static constexpr usize DefaultCollectLimit = ResourceLimits::DefaultCollectBytes;

    ResponseBody(ResponseBody&&) noexcept = default;
    auto operator=(ResponseBody&& other) noexcept -> ResponseBody& {
        if (this != &other) {
            cancel();
            state_ = rstd::move(other.state_);
        }
        return *this;
    }
    ~ResponseBody() { cancel(); }

    auto next() -> coro<Result<Option<Bytes>>> { return read(state_.clone()); }
    auto collect() -> coro<Result<Bytes>> {
        return collect_body(state_.clone(), state_ ? state_->collect_limit : usize());
    }
    auto collect(usize limit) -> coro<Result<Bytes>> { return collect_body(state_.clone(), limit); }
    void cancel() {
        if (state_) state_->backend->cancel();
    }
};

static_assert(lihttpto::BodySource<ResponseBody>);

export class Response {
    friend class Session;
    struct ConstructionKey {};
    Arc<SelectedResponseBackend> backend_;
    lihttpto::ResponseHead       head_;
    usize                        collect_limit_;
    Atomic<bool>                 body_taken_ { false };

    static auto collect_body(Result<ResponseBody> body, usize limit) -> coro<Result<Bytes>> {
        if (body.is_err()) co_return Err(rstd::move(body).unwrap_err());
        co_return co_await body->collect(limit);
    }
    static auto collect_text(Result<ResponseBody> body, usize limit) -> coro<Result<String>> {
        auto data_result = co_await collect_body(rstd::move(body), limit);
        if (data_result.is_err()) co_return Err(rstd::move(data_result).unwrap_err());
        auto data = rstd::move(data_result).unwrap();
        auto text = rstd::str_::from_utf8(data.as_slice());
        if (text.is_err()) co_return Err(Error::Protocol(ProtocolError::InvalidUtf8, nullptr));
        co_return Ok(String::make(text.unwrap()));
    }
    template<lihttpto::BodySink Sink>
    static auto transfer(Result<ResponseBody> body, Sink& sink)
        -> coro<rstd::Result<u64, lihttpto::BodyTransferError<Error, typename Sink::Error>>> {
        using TransferError = lihttpto::BodyTransferError<Error, typename Sink::Error>;
        if (body.is_err()) co_return Err(TransferError::Source(rstd::move(body).unwrap_err()));
        co_return co_await lihttpto::transfer_body(*body, sink);
    }
    static auto make(SelectedResponseBackend backend, ResourceLimits limits)
        -> Result<Arc<Response>> {
        auto head = backend.head();
        if (head.is_none()) return Err(Error::InvalidState("response head is unavailable"));
        auto parsed = (*head)->clone().into_response();
        if (parsed.is_err()) return Err(Error::Protocol(ProtocolError::InvalidStatusLine, nullptr));
        return Ok(Arc<Response>::make(ConstructionKey {},
                                      rstd::move(backend),
                                      rstd::move(parsed).unwrap(),
                                      limits.collect_bytes));
    }

public:
    Response(const Response&)                    = delete;
    auto operator=(const Response&) -> Response& = delete;

    Response(ConstructionKey, SelectedResponseBackend backend, lihttpto::ResponseHead head,
             usize collect_limit)
        : backend_(Arc<SelectedResponseBackend>::make(rstd::move(backend))),
          head_(rstd::move(head)),
          collect_limit_(collect_limit) {}

    auto head() const -> const lihttpto::ResponseHead& { return head_; }
    auto header() const -> const lihttpto::Headers& { return head_.headers; }
    auto code() const -> Option<i32> { return Some(as_cast<i32>(head_.status.value())); }
    auto trailers() const -> Option<ref<lihttpto::Headers>> { return backend_->trailers(); }
    auto request() const -> const Request& { return backend_->request(); }
    auto is_finished() const -> bool { return backend_->is_finished(); }
    void cancel() { backend_->cancel(); }

    auto take_body() -> Result<ResponseBody> {
        if (body_taken_.exchange(true))
            return Err(Error::InvalidState("response body was already taken"));
        return Ok(ResponseBody(backend_.clone(), collect_limit_));
    }
    auto bytes() -> coro<Result<Bytes>> { return bytes(collect_limit_); }
    auto bytes(usize limit) -> coro<Result<Bytes>> { return collect_body(take_body(), limit); }
    auto text() -> coro<Result<String>> { return text(collect_limit_); }
    auto text(usize limit) -> coro<Result<String>> { return collect_text(take_body(), limit); }
    template<lihttpto::BodySink Sink>
    auto read_to_stream(Sink& sink)
        -> coro<rstd::Result<u64, lihttpto::BodyTransferError<Error, typename Sink::Error>>> {
        return transfer(take_body(), sink);
    }
    auto set_cookies() const -> rstd::Result<Vec<lihttpto::SetCookie>, lihttpto::CookieError> {
        auto cookies = Vec<lihttpto::SetCookie>::make();
        for (const auto& value : header().get_all("set-cookie"_str)) {
            auto parsed = lihttpto::SetCookie::parse_bytes(value->as_slice());
            if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
            cookies.push(rstd::move(parsed).unwrap());
        }
        return Ok(rstd::move(cookies));
    }
};
} // namespace ncrequest
