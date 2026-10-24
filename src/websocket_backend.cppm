export module ncrequest:websocket_backend;
export import :error;
export import :upgrade;
export import liwebsocketo;
using namespace rstd::prelude;
export namespace ncrequest
{
struct WebSocketLimit {
    usize maximum {}, observed {};
};
class WebSocketError {
public:
    enum Code
    {
        Busy,
        Closed,
        Cancelled,
        Transport,
        Protocol,
        Timeout,
        Overflow,
        Random,
        QueueFull,
        FrameTooLarge,
        MessageTooLarge,
        ReceiveMessagesFull,
        ReceiveBytesFull,
        CallbackExecutorClosed,
        IncomingLimit
    };

private:
    Code                               code_;
    Option<WebSocketLimit>             limit_;
    Option<lifetchto::Error>           cause_;
    Option<liwebsocketo::MessageError> protocol_;

public:
    WebSocketError(Code code): code_(code) {}
    WebSocketError(liwebsocketo::WebSocketError code): code_(Code(code)) {}
    WebSocketError(Code code, usize maximum, usize observed)
        : code_(code), limit_(Some(WebSocketLimit { maximum, observed })) {}
    auto code() const -> Code { return code_; }
    auto limit() const -> Option<WebSocketLimit> { return limit_; }
    auto protocol_error() const -> Option<liwebsocketo::MessageError> { return protocol_; }
    auto with_protocol(Option<liwebsocketo::MessageError> error) && -> WebSocketError {
        protocol_ = error;
        return rstd::move(*this);
    }
    auto cause() const -> const Option<lifetchto::Error>& { return cause_; }
    auto with_cause(lifetchto::Error cause) && -> WebSocketError {
        cause_ = Some(rstd::move(cause));
        return rstd::move(*this);
    }
    auto clone() const -> WebSocketError {
        auto error      = WebSocketError(code_);
        error.limit_    = limit_;
        error.protocol_ = protocol_;
        if (cause_.is_some()) error.cause_ = Some(cause_->clone());
        return error;
    }
    auto operator==(Code code) const -> bool { return code_ == code; }
    auto operator==(const WebSocketError& other) const -> bool { return code_ == other.code_; }
};
template<class T>
using WebSocketResult = rstd::Result<T, WebSocketError>;
struct WebSocketOptions : liwebsocketo::WebSocketOptions {
private:
    using liwebsocketo::WebSocketOptions::retain_received;

public:
    UpgradeOptions upgrade;
    usize          send_messages { 16 }, send_bytes { 1048576 };
    WebSocketOptions() { limits.frame_bytes = usize(1048576); }
};
struct WebSocketCapabilities {
    bool supported {}, handshake_response {}, handshake_limits {}, peer_close_confirmation {},
        receive_backpressure {}, receive_limit_details { true }, frame_sized_read_window {};
};
struct WebSocketConnectFailure {
    Option<lifetchto::Error>               admission;
    Option<UpgradeFailure>                 upgrade;
    Option<liwebsocketo::HandshakeError>   handshake;
    Option<WebSocketError>                 connection;
    Option<Arc<lifetchto::TransferReport>> report;
    Option<lifetchto::Error>               transport;
};
struct RejectedMessage {
    WebSocketError        error;
    liwebsocketo::Message input;
};
struct WebSocketEndpoint {
    template<class Self, class = void>
    struct Api {
        using Trait = WebSocketEndpoint;
        auto protocol() const -> Option<String> { return rstd::trait_call<0>(this); }
        auto send(liwebsocketo::Message message) const
            -> rstd::async::Completion<liwebsocketo::WebSocketResult<empty>> {
            return rstd::trait_call<1>(this, rstd::move(message));
        }
        auto receive() const
            -> rstd::async::Completion<liwebsocketo::WebSocketResult<liwebsocketo::Message>> {
            return rstd::trait_call<2>(this);
        }
        auto close(Bytes payload) const
            -> rstd::async::Completion<liwebsocketo::WebSocketResult<empty>> {
            return rstd::trait_call<3>(this, rstd::move(payload));
        }
        void cancel() const { rstd::trait_call<4>(this); }
        auto completion() const -> lifetchto::TransferControl { return rstd::trait_call<5>(this); }
        auto shutdown() const -> coro<lifetchto::Result<empty>> {
            return rstd::trait_call<6>(this);
        }
        auto has_receiver() const -> bool { return rstd::trait_call<7>(this); }
        void release_received() const { rstd::trait_call<8>(this); }
        auto failure() const -> Option<WebSocketError> { return rstd::trait_call<9>(this); }
        void discard_received() const { rstd::trait_call<10>(this); }
        auto has_messages() const -> bool { return rstd::trait_call<11>(this); }
    };
    template<class T>
    using Funcs =
        rstd::TraitFuncs<&T::protocol, &T::send, &T::receive, &T::close, &T::cancel, &T::completion,
                         &T::shutdown, &T::has_receiver, &T::release_received, &T::failure,
                         &T::discard_received, &T::has_messages>;
};
struct WebSocketBackend {
    template<class Self, class = void>
    struct Api {
        using Trait = WebSocketBackend;
        auto capabilities() const -> WebSocketCapabilities { return rstd::trait_call<0>(this); }
        auto connect(lifetchto::ResourceContext resources, lifetchto::http::ConnectionPlan plan,
                     lihttpto::Headers headers, Vec<String> protocols,
                     WebSocketOptions options) const
            -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>> {
            return rstd::trait_call<1>(this,
                                       rstd::move(resources),
                                       rstd::move(plan),
                                       rstd::move(headers),
                                       rstd::move(protocols),
                                       options);
        }
    };
    template<class T>
    using Funcs = rstd::TraitFuncs<&T::capabilities, &T::connect>;
};
} // namespace ncrequest

export namespace rstd
{
template<>
struct Impl<clone::Clone, ncrequest::WebSocketError>
    : DefaultInImpl<clone::Clone, ncrequest::WebSocketError> {
    auto clone() const { return this->self().clone(); }
};
template<>
struct Impl<fmt::Display, ncrequest::WebSocketError> : ImplBase<ncrequest::WebSocketError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        auto limit = this->self().limit();
        if (limit.is_some())
            return f.write_fmt(fmt::Arguments::make("WebSocket error {}: limit {}, observed {}",
                                                    i32(this->self().code()),
                                                    limit->maximum,
                                                    limit->observed));
        return f.write_fmt(fmt::Arguments::make("WebSocket error {}", i32(this->self().code())));
    }
};
template<>
struct Impl<fmt::Debug, ncrequest::WebSocketError> : Impl<fmt::Display, ncrequest::WebSocketError> {
};
template<>
struct Impl<error::Error, ncrequest::WebSocketError>
    : DefaultInImpl<error::Error, ncrequest::WebSocketError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& cause = this->self().cause();
        if (cause.is_some()) return Some(dyn<error::Error>::from_ref(*cause));
        return None();
    }
};
} // namespace rstd
