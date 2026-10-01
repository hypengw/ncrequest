export module ncrequest:options;
export import :error;
export import :session_share;
export import :proxy;
export import :timeout;
export import :tls;
export import :redirect_options;
export import rstd;

using namespace rstd::prelude;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::time::Duration;

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

export struct TcpOptions {
    bool keepalive {};
    i64  keepidle { 120 };
    i64  keepintvl { 60 };
};

export struct ShareOptions {
    Option<SessionShare> share;
    auto                 clone() const -> ShareOptions { return { share.clone() }; }
};

export struct RequestOptions {
    Option<Endpoint>        endpoint;
    Option<TimeoutOptions>  timeout;
    Option<ProxyOptions>    proxy;
    Option<TcpOptions>      tcp;
    Option<TlsOptions>      tls;
    Option<ShareOptions>    share;
    Option<RedirectOptions> redirect;

    auto clone() const -> RequestOptions {
        auto out = RequestOptions {};
        if (endpoint.is_some()) out.endpoint = Some(endpoint->clone());
        if (timeout.is_some()) out.timeout = Some(TimeoutOptions(*timeout));
        if (proxy.is_some()) out.proxy = Some(proxy->clone());
        if (tcp.is_some()) out.tcp = Some(TcpOptions(*tcp));
        if (tls.is_some()) out.tls = Some(tls->clone());
        if (share.is_some()) out.share = Some(share->clone());
        if (redirect.is_some()) out.redirect = Some(RedirectOptions(*redirect));
        return out;
    }
};

export struct SessionOptions {
    Endpoint        endpoint;
    TimeoutOptions  timeout;
    ProxyOptions    proxy;
    TcpOptions      tcp;
    TlsOptions      tls;
    ShareOptions    share;
    RedirectOptions redirect;
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
        values.timeout  = request.timeout.is_some() ? *request.timeout : session.timeout;
        values.proxy    = request.proxy.is_some() ? request.proxy->clone() : session.proxy.clone();
        values.tcp      = request.tcp.is_some() ? *request.tcp : session.tcp;
        values.tls      = request.tls.is_some() ? request.tls->clone() : session.tls.clone();
        values.share    = request.share.is_some() ? request.share->clone() : session.share.clone();
        values.redirect = request.redirect.is_some() ? *request.redirect : session.redirect;
        auto timeout    = values.timeout.validate();
        if (timeout.is_err()) return Err(rstd::move(timeout).unwrap_err());
        auto tls = values.tls.validate();
        if (tls.is_err()) return Err(rstd::move(tls).unwrap_err());
        if (values.tcp.keepidle < i64() || values.tcp.keepintvl < i64())
            return Err(Error::InvalidState("TCP intervals cannot be negative"));
        if (values.endpoint.socket_path().is_some()) {
            if (values.proxy.mode() == ProxyOptions::Mode::Explicit)
                return Err(Error::InvalidState("Unix socket endpoint conflicts with proxy"));
            values.proxy = ProxyOptions::disabled();
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
    auto redirect() const -> const RedirectOptions& { return values_.redirect; }
    auto share() const -> const ShareOptions& { return values_.share; }

    auto clone() const -> EffectiveOptions {
        auto out     = SessionOptions {};
        out.endpoint = values_.endpoint.clone();
        out.timeout  = values_.timeout;
        out.proxy    = values_.proxy.clone();
        out.tcp      = values_.tcp;
        out.tls      = values_.tls.clone();
        out.share    = values_.share.clone();
        out.redirect = values_.redirect;
        return EffectiveOptions(rstd::move(out));
    }
    auto remaining_after(Duration elapsed) const -> Result<EffectiveOptions> {
        auto out = clone();
        if (auto total = values_.timeout.total.duration(); total.is_some()) {
            auto remaining = total->checked_sub(elapsed);
            if (remaining.is_none() || remaining->is_zero()) return Err(Error::Timeout());
            out.values_.timeout.total = TimeoutLimit::after(*remaining);
        }
        return Ok(rstd::move(out));
    }
};

} // namespace ncrequest
