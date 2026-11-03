module;
#include <rstd/enum.hpp>
export module ncrequest:error;
export import lifetchto;
using namespace rstd::prelude;
using namespace rstd::literals;
export namespace ncrequest
{
using rstd::async::coro;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
class Error {
    RSTD_ENUM(Error, (Transport, (lifetchto::Error cause;)),
              (Decode, (Box<rstd::dyn<rstd::error::Error>> cause;)),
              (InvalidOptions, (String message;)), (TooLarge, (usize limit;)), (Busy), (BodyTaken),
              (Consumed), (Closed))
public:
    template<class E>
    static auto decode(E cause) -> Error {
        return Decode(Box<rstd::dyn<rstd::error::Error>>::make(rstd::move(cause)));
    }
};
template<class T>
using Result = rstd::Result<T, Error>;
} // namespace ncrequest
export namespace rstd
{
template<>
struct Impl<fmt::Display, ncrequest::Error> : ImplBase<ncrequest::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& e = this->self();
        if (e.is_Transport())
            return f.write_fmt(fmt::Arguments::make("{}", e.as_Transport().cause));
        if (e.is_Decode())
            return f.write_fmt(fmt::Arguments::make("{}", e.as_Decode().cause.deref()));
        if (e.is_InvalidOptions()) return f.write_str(e.as_InvalidOptions().message.as_str());
        if (e.is_TooLarge())
            return f.write_fmt(
                fmt::Arguments::make("collection exceeds {} bytes", e.as_TooLarge().limit));
        if (e.is_Busy()) return f.write_str("response body already has a reader"_str);
        if (e.is_BodyTaken()) return f.write_str("response body was already taken"_str);
        if (e.is_Consumed()) return f.write_str("response body was consumed"_str);
        return f.write_str("request handle is closed"_str);
    }
};
template<>
struct Impl<fmt::Debug, ncrequest::Error> : Impl<fmt::Display, ncrequest::Error> {};
template<>
struct Impl<error::Error, ncrequest::Error> : DefaultInImpl<error::Error, ncrequest::Error> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& e = this->self();
        if (e.is_Transport()) return Some(dyn<error::Error>::from_ref(e.as_Transport().cause));
        if (e.is_Decode()) return Some(e.as_Decode().cause.deref());
        return None();
    }
};
} // namespace rstd
