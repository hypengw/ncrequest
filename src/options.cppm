export module ncrequest:options;
export import :error;
export import :session_share;
export import ncrequest.type;

using namespace rstd::prelude;
using rstd::path::Path;
using rstd::path::PathBuf;

namespace ncrequest
{

export class Endpoint {
    Option<PathBuf> socket_;

public:
    static auto network() -> Endpoint { return {}; }
    static auto unix_socket(PathBuf path) -> Result<Endpoint> {
        if (path.is_empty()) return Err(Error::InvalidState("Unix socket path is empty"));
        auto view    = path.as_path();
        auto os_path = view.as_os_str();
        for (auto value : os_path.as_encoded_bytes()) {
            if (value == u8()) return Err(Error::InvalidState("Unix socket path contains NUL"));
        }
        auto endpoint    = Endpoint {};
        endpoint.socket_ = Some(rstd::move(path));
        return Ok(rstd::move(endpoint));
    }
    auto socket_path() const -> Option<ref<Path>> {
        if (socket_.is_none()) return None<ref<Path>>();
        return Some(socket_->as_path());
    }
    auto clone() const -> Endpoint {
        auto endpoint = Endpoint {};
        if (socket_.is_some()) endpoint.socket_ = Some(socket_->clone());
        return endpoint;
    }
};

export struct TimeoutOptions {
    i64 low_speed { 30 };
    i64 connect_timeout { 180 };
    i64 transfer_timeout {};
};

export struct ProxyOptions {
    enum class Type
    {
        HTTP    = 0,
        HTTPS2  = 3,
        SOCKS4  = 4,
        SOCKS5  = 5,
        SOCKS4A = 6,
        SOCKS5H = 7
    };
    Type        type { Type::HTTP };
    std::string content;
};

export struct TcpOptions {
    bool keepalive {};
    i64  keepidle { 120 };
    i64  keepintvl { 60 };
};

export struct TlsOptions {
    bool verify_certificate { true };
};

export struct ShareOptions {
    Option<SessionShare> share;
    auto                 clone() const -> ShareOptions { return { share.clone() }; }
};

export struct RequestOptions {
    Option<Endpoint>       endpoint;
    Option<TimeoutOptions> timeout;
    Option<ProxyOptions>   proxy;
    Option<TcpOptions>     tcp;
    Option<TlsOptions>     tls;
    Option<ShareOptions>   share;

    auto clone() const -> RequestOptions {
        auto out = RequestOptions {};
        if (endpoint.is_some()) out.endpoint = Some(endpoint->clone());
        if (timeout.is_some()) out.timeout = Some(TimeoutOptions(*timeout));
        if (proxy.is_some()) out.proxy = Some(ProxyOptions(*proxy));
        if (tcp.is_some()) out.tcp = Some(TcpOptions(*tcp));
        if (tls.is_some()) out.tls = Some(TlsOptions(*tls));
        if (share.is_some()) out.share = Some(share->clone());
        return out;
    }
};

export struct SessionOptions {
    Endpoint       endpoint;
    TimeoutOptions timeout;
    ProxyOptions   proxy;
    TcpOptions     tcp;
    TlsOptions     tls;
    ShareOptions   share;
};

export class EffectiveOptions {
    SessionOptions values_;
    explicit EffectiveOptions(SessionOptions values): values_(rstd::move(values)) {}

public:
    static auto resolve(const SessionOptions& session, const RequestOptions& request)
        -> Result<EffectiveOptions> {
        auto values = SessionOptions {};
        values.endpoint =
            request.endpoint.is_some() ? request.endpoint->clone() : session.endpoint.clone();
        values.timeout = request.timeout.is_some() ? *request.timeout : session.timeout;
        values.proxy   = request.proxy.is_some() ? *request.proxy : session.proxy;
        values.tcp     = request.tcp.is_some() ? *request.tcp : session.tcp;
        values.tls     = request.tls.is_some() ? *request.tls : session.tls;
        values.share   = request.share.is_some() ? request.share->clone() : session.share.clone();
        if (values.timeout.low_speed < i64() || values.timeout.connect_timeout < i64() ||
            values.timeout.transfer_timeout < i64())
            return Err(Error::InvalidState("timeout options cannot be negative"));
        if (values.tcp.keepidle < i64() || values.tcp.keepintvl < i64())
            return Err(Error::InvalidState("TCP intervals cannot be negative"));
        if (values.endpoint.socket_path().is_some()) {
            if (! values.proxy.content.empty())
                return Err(Error::InvalidState("Unix socket endpoint conflicts with proxy"));
            if (values.tcp.keepalive)
                return Err(
                    Error::InvalidState("Unix socket endpoint conflicts with TCP keepalive"));
        }
        return Ok(EffectiveOptions(rstd::move(values)));
    }
    auto endpoint() const -> const Endpoint& { return values_.endpoint; }
    auto timeout() const -> const TimeoutOptions& { return values_.timeout; }
    auto proxy() const -> const ProxyOptions& { return values_.proxy; }
    auto tcp() const -> const TcpOptions& { return values_.tcp; }
    auto tls() const -> const TlsOptions& { return values_.tls; }
    auto share() const -> const ShareOptions& { return values_.share; }
};

} // namespace ncrequest
