module;
#include <QNetworkAccessManager>
export module ncrequest:client.qt_network.backend;
export import :session;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client::qt_network
{
struct BackendState;
}
export namespace ncrequest::client::qt_network
{
struct Options {
    usize                                    window { 65536 }, head_limit { 65536 };
    QNetworkAccessManager*                   manager {};
    lifetchto::http::TlsOptions              tls;
    lifetchto::http::ProxyOptions            proxy;
    lifetchto::http::TcpOptions              tcp;
    Option<lifetchto::http::LowSpeedOptions> low_speed;
    Option<rstd::time::Duration>             connect_timeout;
};
class Error {
    i32    code_;
    String message_;

public:
    Error(i32 code, String message): code_(code), message_(rstd::move(message)) {}
    auto code() const -> i32 { return code_; }
    auto message() const -> ref<str> { return message_.as_str(); }
};
class WebSocketNativeError {
    i32    code_;
    u16    close_code_;
    String message_;

public:
    WebSocketNativeError(i32 code, u16 close_code, String message)
        : code_(code), close_code_(close_code), message_(rstd::move(message)) {}
    auto code() const -> i32 { return code_; }
    auto close_code() const -> u16 { return close_code_; }
    auto message() const -> ref<str> { return message_.as_str(); }
};
class Backend {
    Arc<BackendState>                    state_;
    Arc<lifetchto::ResourceLease<empty>> registration_;
    Backend(Arc<BackendState>, Arc<lifetchto::ResourceLease<empty>>);

public:
    Backend(Backend&&) noexcept;
    Backend(const Backend&) = delete;
    auto operator=(Backend&&) noexcept -> Backend&;
    ~Backend();
    static auto make(lifetchto::ResourceContext, Options = {}) -> lifetchto::Result<Backend>;
    auto        clone() const -> Backend;
    auto        backend() const -> Arc<rstd::dyn<lifetchto::http::ExchangeBackend>>;
    auto        shared_binding() const -> ncrequest::BackendBinding;
    auto        owned_binding() const -> ncrequest::BackendBinding;
    auto        capabilities() const -> lifetchto::http::BackendCapabilities;
    auto        duplex(lifetchto::http::DuplexRequest input) const
        -> rstd::Result<lifetchto::PreparedOperation<lifetchto::DuplexStream>,
                        lifetchto::Rejected<lifetchto::http::DuplexRequest>> {
        return Err(lifetchto::Rejected<lifetchto::http::DuplexRequest> {
            lifetchto::Error::Unsupported(
                "QNetworkAccessManager does not expose a duplex stream"_Str),
            rstd::move(input) });
    }
    auto prepare(lihttpto::RequestParts, lifetchto::http::UploadMetadata,
                 lifetchto::http::RouteChoice) const
        -> lifetchto::Result<lifetchto::http::PreparedExchange>;
    auto operation(lifetchto::http::ExchangeInput) const
        -> rstd::Result<lifetchto::PreparedOperation<lifetchto::http::ResponseMessage>,
                        lifetchto::Rejected<lifetchto::http::ExchangeInput>>;
    void request_close() const;
    auto close() const -> coro<lifetchto::Result<empty>>;
};
} // namespace ncrequest::client::qt_network
export namespace rstd
{
template<>
struct Impl<fmt::Display, ncrequest::client::qt_network::WebSocketNativeError>
    : ImplBase<ncrequest::client::qt_network::WebSocketNativeError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(fmt::Arguments::make("Qt WebSocket error {} close {}: {}",
                                                this->self().code(),
                                                this->self().close_code(),
                                                this->self().message()));
    }
};
template<>
struct Impl<fmt::Debug, ncrequest::client::qt_network::WebSocketNativeError>
    : Impl<fmt::Display, ncrequest::client::qt_network::WebSocketNativeError> {};
template<>
struct Impl<error::Error, ncrequest::client::qt_network::WebSocketNativeError>
    : DefaultInImpl<error::Error, ncrequest::client::qt_network::WebSocketNativeError> {};
template<>
struct Impl<fmt::Display, ncrequest::client::qt_network::Error>
    : ImplBase<ncrequest::client::qt_network::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(fmt::Arguments::make(
            "Qt Network error {}: {}", this->self().code(), this->self().message()));
    }
};
template<>
struct Impl<fmt::Debug, ncrequest::client::qt_network::Error>
    : Impl<fmt::Display, ncrequest::client::qt_network::Error> {};
template<>
struct Impl<error::Error, ncrequest::client::qt_network::Error>
    : DefaultInImpl<error::Error, ncrequest::client::qt_network::Error> {};
template<>
struct Impl<lifetchto::http::ExchangeBackend, ncrequest::client::qt_network::Backend>
    : ImplBase<ncrequest::client::qt_network::Backend> {
    auto prepare(lihttpto::RequestParts p, lifetchto::http::UploadMetadata u,
                 lifetchto::http::RouteChoice r) const {
        return this->self().prepare(rstd::move(p), rstd::move(u), rstd::move(r));
    }
    auto operation(lifetchto::http::ExchangeInput i) const {
        return this->self().operation(rstd::move(i));
    }
    void request_close() const { this->self().request_close(); }
    auto close() const { return this->self().close(); }
    auto capabilities() const { return this->self().capabilities(); }
    auto duplex(lifetchto::http::DuplexRequest input) const {
        return this->self().duplex(rstd::move(input));
    }
};
} // namespace rstd
