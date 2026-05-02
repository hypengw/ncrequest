export module ncrequest:error;
export import rstd;
export import ncrequest.coro;
export import cppstd;

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
struct rstd::Impl<rstd::fmt::Display, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        std::string out;
        switch (this->self().kind()) {
        case ncrequest::Error::Coro: {
            out = std::get<0>(this->self().data).message();
            break;
        }
        }
        return f.write_raw((u8 const*)out.data(), out.size());
    }
};

template<>
struct rstd::Impl<rstd::convert::From<ncrequest::CoroError>, ncrequest::Error> {
    static auto from(ncrequest::CoroError e) -> ncrequest::Error {
        ncrequest::Error out {};
        out.data = e;
        return out;
    };
};