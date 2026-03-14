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

    using data_t = cppstd::variant<CoroError>;
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

using rstd::string::String;
template<>
struct rstd::fmt::formatter<ncrequest::Error> : rstd::fmt::formatter<String> {
    using Error = ncrequest::Error;
    template<typename C>
    auto format(const Error& e, C& ctx) const -> C::iterator {
        using namespace rstd;
        auto out = String::make();
        switch (e.kind()) {
        case Error::Coro: {
            out = fmt::format("{}", cppstd::get<0>(e.data).message());
            break;
        }
        }
        return fmt::formatter<String>::format(out, ctx);
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