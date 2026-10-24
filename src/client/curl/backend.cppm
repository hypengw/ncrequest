export module ncrequest:client.curl.backend;
export import :session;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client::curl
{
struct BackendState;
}
export namespace ncrequest::client::curl
{
struct Options {
    usize                         window { 65536 }, head_limit { 65536 };
    rstd::time::Duration          connect_timeout { rstd::time::Duration::from_secs(u64(10)) };
    lifetchto::http::TlsOptions   tls, proxy_tls;
    lifetchto::http::ProxyOptions proxy;
    lifetchto::http::TcpOptions   tcp;
    Option<lifetchto::http::LowSpeedOptions> low_speed;
};
class Error {
    i32  code_;
    bool multi_ {};

public:
    explicit Error(i32 code): code_(code) {}
    static auto multi(i32 code) -> Error {
        Error value(code);
        value.multi_ = true;
        return value;
    }
    auto code() const -> i32 { return code_; }
    auto is_multi() const -> bool { return multi_; }
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
    auto        shared_binding() const -> ncrequest::BackendBinding {
        return ncrequest::BackendBinding::Shared(backend());
    }
    auto owned_binding() const -> ncrequest::BackendBinding {
        return ncrequest::BackendBinding::Owned(backend());
    }
    auto capabilities() const -> lifetchto::http::BackendCapabilities;
    auto duplex(lifetchto::http::DuplexRequest input) const
        -> rstd::Result<lifetchto::PreparedOperation<lifetchto::DuplexStream>,
                        lifetchto::Rejected<lifetchto::http::DuplexRequest>>;
    auto prepare(lihttpto::RequestParts, lifetchto::http::UploadMetadata,
                 lifetchto::http::RouteChoice) const
        -> lifetchto::Result<lifetchto::http::PreparedExchange>;
    auto operation(lifetchto::http::ExchangeInput) const
        -> rstd::Result<lifetchto::PreparedOperation<lifetchto::http::ResponseMessage>,
                        lifetchto::Rejected<lifetchto::http::ExchangeInput>>;
    void request_close() const;
    auto close() const -> coro<lifetchto::Result<empty>>;
};
} // namespace ncrequest::client::curl
export namespace rstd
{
template<>
struct Impl<fmt::Display, ncrequest::client::curl::Error>
    : ImplBase<ncrequest::client::curl::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(fmt::Arguments::make("curl {}error {}",
                                                this->self().is_multi() ? "multi "_str : ""_str,
                                                this->self().code()));
    }
};
template<>
struct Impl<fmt::Debug, ncrequest::client::curl::Error>
    : Impl<fmt::Display, ncrequest::client::curl::Error> {};
template<>
struct Impl<error::Error, ncrequest::client::curl::Error>
    : DefaultInImpl<error::Error, ncrequest::client::curl::Error> {};
template<>
struct Impl<lifetchto::http::ExchangeBackend, ncrequest::client::curl::Backend>
    : ImplBase<ncrequest::client::curl::Backend> {
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
