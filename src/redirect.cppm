export module ncrequest:redirect;
export import :request;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::time::Instant;

namespace ncrequest
{

class RedirectState {
    EffectiveOptions options_;
    Instant          started_ { Instant::now() };
    u32              hops_ {};

public:
    explicit RedirectState(EffectiveOptions options): options_(rstd::move(options)) {}

    auto next(const Request& request, const lihttpto::MessageHead& head)
        -> Result<Option<PreparedRequest>> {
        if (! options_.redirect().enabled()) return Ok(None<PreparedRequest>());
        auto status = head.status_code();
        if (status.is_none()) return Ok(None<PreparedRequest>());
        auto code = status->to_primitive();
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
            return Ok(None<PreparedRequest>());
        auto locations = head.headers().get_all("location"_str);
        if (locations.is_empty()) return Ok(None<PreparedRequest>());
        if (locations.len() != usize(1))
            return Err(Error::Protocol(ProtocolError::InvalidRedirect, nullptr));
        if (hops_ >= options_.redirect().max_hops())
            return Err(Error::Protocol(ProtocolError::RedirectLimitExceeded, nullptr));
        auto location = locations[usize()]->to_str();
        if (location.is_err()) return Err(Error::Protocol(ProtocolError::InvalidRedirect, nullptr));
        auto target = request.url_info().resolve(*location);
        if (target.is_err() || target->userinfo().is_some())
            return Err(Error::Protocol(ProtocolError::InvalidRedirect, nullptr));
        if (! request.url_info().same_http_origin(*target))
            return Err(Error::Protocol(ProtocolError::RedirectOriginChanged, nullptr));
        auto method       = request.method().as_ref();
        auto use_get      = (code == 303 && method != "HEAD"_str) ||
                            ((code == 301 || code == 302) && method == "POST"_str);
        auto next_request = request.redirected(rstd::move(target).unwrap(), use_get);
        if (next_request.is_err()) return Err(rstd::move(next_request).unwrap_err());
        auto remaining = options_.remaining_after(started_.elapsed());
        if (remaining.is_err()) return Err(rstd::move(remaining).unwrap_err());
        auto prepared = PreparedRequest::prepare(rstd::move(next_request).unwrap(),
                                                 rstd::move(remaining).unwrap());
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err());
        ++hops_;
        return Ok(Some(rstd::move(prepared).unwrap()));
    }
};

} // namespace ncrequest
