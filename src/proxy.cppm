export module ncrequest:proxy;
export import :error;
export import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace ncrequest
{

export class ProxyOptions {
public:
    enum class Mode
    {
        System,
        Disabled,
        Explicit
    };
    enum class Type
    {
        HTTP,
        HTTPS,
        SOCKS4,
        SOCKS4A,
        SOCKS5,
        SOCKS5H
    };

    static auto system() -> ProxyOptions { return {}; }
    static auto disabled() -> ProxyOptions {
        auto out  = ProxyOptions {};
        out.mode_ = Mode::Disabled;
        return out;
    }
    static auto explicit_proxy(lihttpto::Url url) -> Result<ProxyOptions> {
        auto scheme = url.scheme();
        auto host   = url.host();
        if (scheme.is_none() || host.is_none() || host->is_empty())
            return Err(Error::InvalidState("proxy URL requires a scheme and host"));
        if (url.userinfo().is_some())
            return Err(Error::Unsupported("proxy URL userinfo is not supported"));
        if ((! url.path().is_empty() && url.path() != "/"_str) || url.query().is_some() ||
            url.fragment().is_some())
            return Err(Error::InvalidState("proxy URL cannot contain a path, query or fragment"));
        auto normalized = String::make(*scheme);
        normalized.as_mut_str().make_ascii_lowercase();
        auto out = ProxyOptions {};
        if (normalized == "http"_str)
            out.type_ = Type::HTTP;
        else if (normalized == "https"_str)
            out.type_ = Type::HTTPS;
        else if (normalized == "socks4"_str)
            out.type_ = Type::SOCKS4;
        else if (normalized == "socks4a"_str)
            out.type_ = Type::SOCKS4A;
        else if (normalized == "socks5"_str)
            out.type_ = Type::SOCKS5;
        else if (normalized == "socks5h"_str)
            out.type_ = Type::SOCKS5H;
        else
            return Err(Error::Unsupported("unsupported proxy URL scheme"));
        if (auto port = url.port(); port.is_some()) {
            auto parsed = rstd::from_str<u16>(*port);
            if (parsed.is_err() || parsed.unwrap() == u16())
                return Err(Error::InvalidState("proxy port must be between 1 and 65535"));
            out.port_ = parsed.unwrap();
        }
        out.mode_ = Mode::Explicit;
        out.url_  = Some(rstd::move(url));
        return Ok(rstd::move(out));
    }

    auto mode() const -> Mode { return mode_; }
    auto type() const -> Option<Type> {
        return mode_ == Mode::Explicit ? Some(Type(type_)) : None<Type>();
    }
    auto url() const -> Option<ref<lihttpto::Url>> {
        if (url_.is_none()) return None();
        return Some(ref<lihttpto::Url>::from_raw_parts(&*url_));
    }
    auto port() const -> Option<u16> {
        return mode_ == Mode::Explicit ? Some(u16(port_)) : None<u16>();
    }
    auto clone() const -> ProxyOptions {
        auto out  = ProxyOptions {};
        out.mode_ = mode_;
        out.type_ = type_;
        out.port_ = port_;
        if (url_.is_some()) out.url_ = Some(url_->clone());
        return out;
    }

private:
    Mode                  mode_ { Mode::System };
    Type                  type_ { Type::HTTP };
    u16                   port_ { 1080 };
    Option<lihttpto::Url> url_;
};

} // namespace ncrequest
