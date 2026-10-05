export module ncrequest:upgrade;
export import :unix_duplex;
export import lihttpto;
import rstd;

using namespace rstd::prelude;
using rstd::async::Completion;
using rstd::async::coro;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::time::Duration;
using rstd::time::Instant;

export namespace ncrequest
{
struct UpgradeFailure {
    Option<Error>                    transport {};
    Option<lihttpto::HttpParseError> parse {};
    Option<lihttpto::UpgradeError>   protocol {};
    Option<lihttpto::ResponseHead>   response {};
    Option<lihttpto::DecodeError>    body {};
    Bytes                            error_body;
    bool                             body_complete {};
};
struct UpgradeOptions {
    DuplexOptions duplex;
    Duration      handshake_timeout { Duration::from_secs(u64(10)) };
    usize         header_bytes { 65536 };
    usize         error_body_bytes { 65536 };
};

class UpgradeConnection {
    struct State {
        Box<UnixDuplex>                  wire;
        lihttpto::ResponseHead           response;
        Bytes                            pending;
        rstd::sync::atomic::Atomic<bool> reading { false }, cancelled { false };
        State(Box<UnixDuplex> stream, lihttpto::ResponseHead head, Bytes remainder)
            : wire(rstd::move(stream)),
              response(rstd::move(head)),
              pending(rstd::move(remainder)) {}
    };
    Arc<State> state_;
    explicit UpgradeConnection(Arc<State> state): state_(rstd::move(state)) {}
    struct ReadGuard {
        Arc<State> state;
        bool       complete {};
        ~ReadGuard() {
            if (! complete) {
                state->cancelled.store(true);
                state->wire->cancel();
            }
            state->reading.store(false);
        }
    };
    template<class T>
    static auto await_io(Completion<Result<T>> completion) -> coro<Result<T>> {
        auto result = co_await rstd::move(completion);
        if (result.is_err()) co_return Err(Error::Canceled());
        co_return rstd::move(result).unwrap();
    }
    template<class T>
    static auto bounded(Completion<Result<T>> completion, Instant started, Duration budget)
        -> coro<Result<T>> {
        auto elapsed = started.elapsed();
        if (elapsed >= budget) co_return Err(Error::Timeout());
        auto task =
            rstd::async::AbortOnDropHandle(rstd::async::spawn(await_io(rstd::move(completion))));
        auto result = co_await rstd::async::timeout(rstd::future::by_ref(task), budget - elapsed);
        if (result.is_err()) {
            task.abort();
            (void)co_await rstd::future::by_ref(task);
            co_return Err(Error::Timeout());
        }
        auto joined = rstd::move(result).unwrap();
        if (joined.is_err()) co_return Err(Error::Canceled());
        co_return rstd::move(joined).unwrap();
    }
    static auto read(Arc<State> state) -> coro<Result<Bytes>> {
        if (! state || state->reading.exchange(true))
            co_return Err(Error::InvalidState("read already pending"));
        ReadGuard guard { state.clone() };
        if (state->cancelled.load()) co_return Err(Error::Canceled());
        if (! state->pending.is_empty()) {
            guard.complete = true;
            co_return Ok(rstd::exchange(state->pending, Bytes {}));
        }
        auto received  = co_await await_io(state->wire->read());
        guard.complete = received.is_ok();
        co_return rstd::move(received);
    }
    static auto collect_error(UnixDuplex& wire, const lihttpto::UpgradeRequest& request,
                              UpgradeFailure failure, Bytes pending, Instant started,
                              UpgradeOptions options) -> coro<UpgradeFailure> {
        auto framing = request.body_framing(*failure.response);
        if (framing.is_err()) {
            failure.body = Some(framing.unwrap_err());
            co_return rstd::move(failure);
        }
        lihttpto::BodyLimits limits;
        limits.data_bytes     = u64(options.error_body_bytes.to_primitive());
        limits.metadata_bytes = usize(16384);
        lihttpto::BodyDecoder decoder(*framing, limits);
        Vec<u8>               collected;
        for (;;) {
            auto progress = decoder.feed(pending.as_slice());
            if (progress.is_err()) {
                failure.body = Some(progress.unwrap_err());
                break;
            }
            collected.extend_from_slice(progress->data);
            pending.advance(progress->consumed);
            if (progress->status == lihttpto::DecodeStatus::Complete) {
                failure.body_complete = true;
                break;
            }
            if (! pending.is_empty()) continue;
            auto next = co_await bounded(wire.read(), started, options.handshake_timeout);
            if (next.is_err()) {
                failure.transport = Some(rstd::move(next).unwrap_err());
                break;
            }
            pending = rstd::move(next).unwrap();
            if (pending.is_empty()) {
                auto finished = decoder.finish();
                if (finished.is_err())
                    failure.body = Some(finished.unwrap_err());
                else
                    failure.body_complete = true;
                break;
            }
        }
        failure.error_body = Bytes::copy_from_slice(collected.as_slice());
        co_return rstd::move(failure);
    }

public:
    UpgradeConnection(const UpgradeConnection&)                    = delete;
    auto operator=(const UpgradeConnection&) -> UpgradeConnection& = delete;
    UpgradeConnection(UpgradeConnection&&) noexcept                = default;
    auto operator=(UpgradeConnection&& other) noexcept -> UpgradeConnection& {
        if (this != &other) {
            cancel();
            state_ = rstd::move(other.state_);
        }
        return *this;
    }
    ~UpgradeConnection() { cancel(); }

    static auto open(Endpoint endpoint, lihttpto::UpgradeRequest request,
                     UpgradeOptions options = {})
        -> coro<rstd::Result<UpgradeConnection, UpgradeFailure>> {
        if (options.handshake_timeout.is_zero() || options.header_bytes == usize() ||
            options.header_bytes > usize(65536) || options.error_body_bytes > usize(65536) ||
            request.bytes().len() > usize(65536))
            co_return Err(UpgradeFailure {
                .transport = Some(Error::InvalidState("invalid upgrade limits")) });
        auto started = Instant::now();
        auto made    = UnixDuplex::make(rstd::move(endpoint), options.duplex);
        if (made.is_err())
            co_return Err(UpgradeFailure { .transport = Some(rstd::move(made).unwrap_err()) });
        auto wire      = rstd::move(made).unwrap();
        auto connected = co_await bounded(wire->connected(), started, options.handshake_timeout);
        if (connected.is_err())
            co_return Err(UpgradeFailure { .transport = Some(rstd::move(connected).unwrap_err()) });
        auto sent =
            co_await bounded(wire->write(Bytes::copy_from_slice(request.bytes().as_slice())),
                             started,
                             options.handshake_timeout);
        if (sent.is_err())
            co_return Err(UpgradeFailure { .transport = Some(rstd::move(sent).unwrap_err()) });
        lihttpto::Http1HeadParser parser(false, options.header_bytes);
        Bytes                     bytes;
        usize                     header_total {};
        unsigned                  informational {};
        for (;;) {
            if (bytes.is_empty()) {
                auto received = co_await bounded(wire->read(), started, options.handshake_timeout);
                if (received.is_err())
                    co_return Err(
                        UpgradeFailure { .transport = Some(rstd::move(received).unwrap_err()) });
                bytes = rstd::move(received).unwrap();
                if (bytes.is_empty())
                    co_return Err(UpgradeFailure { .transport = Some(Error::Protocol(
                                                       ProtocolError::UnexpectedEof, nullptr)) });
            }
            auto parsed = parser.push(bytes.as_slice());
            if (parsed.is_err())
                co_return Err(UpgradeFailure { .parse = Some(rstd::move(parsed).unwrap_err()) });
            if (parsed->is_NeedMore()) {
                bytes = {};
                continue;
            }
            auto completed = rstd::move(parsed).unwrap().as_Complete();
            auto head      = rstd::move(completed.head).into_response();
            if (head.is_err())
                co_return Err(UpgradeFailure { .parse = Some(rstd::move(head).unwrap_err()) });
            auto response = rstd::move(head).unwrap();
            bytes.advance(completed.input_consumed);
            auto disposition = request.classify(response);
            if (disposition.is_err())
                co_return Err(UpgradeFailure { .protocol = Some(disposition.unwrap_err()),
                                               .response = Some(rstd::move(response)) });
            if (*disposition == lihttpto::UpgradeDisposition::Informational) {
                header_total += completed.consumed;
                if (++informational > 8 || header_total >= options.header_bytes)
                    co_return Err(
                        UpgradeFailure { .protocol = Some(lihttpto::UpgradeError::TooLarge) });
                parser = lihttpto::Http1HeadParser(false, options.header_bytes - header_total);
                continue;
            }
            if (*disposition == lihttpto::UpgradeDisposition::Rejected) {
                auto failure = co_await collect_error(
                    *wire,
                    request,
                    UpgradeFailure { .protocol = Some(lihttpto::UpgradeError::RejectedStatus),
                                     .response = Some(rstd::move(response)) },
                    rstd::move(bytes),
                    started,
                    options);
                co_return Err(rstd::move(failure));
            }
            co_return Ok(UpgradeConnection {
                Arc<State>::make(rstd::move(wire), rstd::move(response), rstd::move(bytes)) });
        }
    }
    auto response() const -> const lihttpto::ResponseHead& { return state_->response; }
    auto read() -> coro<Result<Bytes>> { return read(state_.clone()); }
    auto write(Bytes bytes) -> coro<Result<usize>> {
        return await_io(state_->wire->write(rstd::move(bytes)));
    }
    auto close_input() -> coro<Result<empty>> { return await_io(state_->wire->close_input()); }
    void cancel() {
        if (state_) {
            state_->cancelled.store(true);
            state_->wire->cancel();
        }
    }
};
} // namespace ncrequest
