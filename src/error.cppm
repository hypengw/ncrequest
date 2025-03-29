module;
#include <variant>
#include <format>
export module ncrequest:error;
export import rstd;
export import ncrequest.coro;

namespace ncrequest
{

struct Error {
    enum ErrorKind
    {
        Coro = 0
    };

    using data_t = std::variant<CoroError>;
    data_t data;

    template<ErrorKind E>
    bool is() const {
        return data.index() == E;
    }

    auto kind() const -> ErrorKind { return (ErrorKind)data.index(); }
};

export template<typename T>
using Result = rstd::Result<T, Error>;

} // namespace ncrequest

template<>
struct std::formatter<ncrequest::Error> : std::formatter<std::string_view> {
    using Error = ncrequest::Error;
    template<typename C>
    auto format(const Error& e, C& ctx) const -> C::iterator {
        std::string out;
        switch (e.kind()) {
        case Error::Coro: {
            out = std::format("{}", std::get<0>(e.data).message());
            break;
        }
        }
        return std::formatter<std::string_view>::format(out, ctx);
    }
};

export template<>
struct rstd::Impl<rstd::convert::From<ncrequest::CoroError>, ncrequest::Error> {
    static auto from(ncrequest::CoroError e) -> ncrequest::Error {
        ncrequest::Error out {};
        out.data = e;
        return out;
    };
};