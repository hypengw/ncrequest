module;
#include <rstd/enum.hpp>

export module ncrequest:error;
export import rstd;
export import rstd.error;
export import ncrequest.curl;
export import cppstd;

namespace ncrequest
{

export enum class ProtocolError
{
    InvalidStatusLine,
    InvalidHeaderLine,
    HeaderTooLarge,
    BodyTooLarge,
    UnexpectedEof,
};

export enum class ErrorKind
{
    Curl,
    Client,
    Io,
    Protocol,
    Canceled,
    InvalidState,
};

export enum class ClientBackend
{
    QtNetwork,
};

export struct ClientError {
    ClientBackend backend;
    i32           code;
    std::string   message;
};

#define NCREQUEST_ERROR_VARIANTS(V)                    \
    V(Curl, (curl::CURLcode code;))                    \
    V(Client, (ClientError error;))                    \
    V(Io, (rstd::io::error::Error error;))             \
    V(Protocol, (ProtocolError kind; const char* msg;)) \
    V(Canceled, ())                                    \
    V(InvalidState, (const char* msg;))

export struct Error {
    RSTD_ENUM_BODY_WITH_DEFAULT(
        Error, NCREQUEST_ERROR_VARIANTS, InvalidState, "uncategorized ncrequest error")

    auto kind() const noexcept -> ErrorKind {
        switch (tag()) {
        case Tag::Curl: return ErrorKind::Curl;
        case Tag::Client: return ErrorKind::Client;
        case Tag::Io: return ErrorKind::Io;
        case Tag::Protocol: return ErrorKind::Protocol;
        case Tag::Canceled: return ErrorKind::Canceled;
        case Tag::InvalidState: return ErrorKind::InvalidState;
        }
        return ErrorKind::InvalidState;
    }
};

export template<typename T>
using Result = rstd::Result<T, Error>;

#undef NCREQUEST_ERROR_VARIANTS

constexpr auto protocol_error_message(ProtocolError kind) noexcept -> const char* {
    switch (kind) {
    case ProtocolError::InvalidStatusLine: return "invalid HTTP status line";
    case ProtocolError::InvalidHeaderLine: return "invalid HTTP header line";
    case ProtocolError::HeaderTooLarge: return "HTTP header too large";
    case ProtocolError::BodyTooLarge: return "HTTP body too large";
    case ProtocolError::UnexpectedEof: return "unexpected EOF";
    }
    return "protocol error";
}

} // namespace ncrequest

template<>
struct rstd::Impl<rstd::fmt::Display, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        auto& e = this->self();
        switch (e.tag()) {
        case ncrequest::Error::Tag::Curl: {
            auto* msg = curl::curl_easy_strerror(e.as_Curl().code);
            return f.write_raw((u8 const*)msg, rstd::strlen(msg));
        }
        case ncrequest::Error::Tag::Client: {
            auto& msg = e.as_Client().error.message;
            return f.write_raw((u8 const*)msg.data(), msg.size());
        }
        case ncrequest::Error::Tag::Io: {
            return rstd::as<rstd::fmt::Display>(e.as_Io().error).fmt(f);
        }
        case ncrequest::Error::Tag::Protocol: {
            auto& payload = e.as_Protocol();
            auto* msg = payload.msg != nullptr ? payload.msg
                                               : ncrequest::protocol_error_message(payload.kind);
            return f.write_raw((u8 const*)msg, rstd::strlen(msg));
        }
        case ncrequest::Error::Tag::Canceled: {
            constexpr std::string_view msg { "operation canceled" };
            return f.write_raw((u8 const*)msg.data(), msg.size());
        }
        case ncrequest::Error::Tag::InvalidState: {
            auto* msg = e.as_InvalidState().msg;
            if (msg == nullptr) msg = "invalid ncrequest state";
            return f.write_raw((u8 const*)msg, rstd::strlen(msg));
        }
        }
        return false;
    }
};

template<>
struct rstd::Impl<rstd::fmt::Debug, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return rstd::as<rstd::fmt::Display>(this->self()).fmt(f);
    }
};

template<>
struct rstd::Impl<rstd::error::Error, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {};

template<>
struct rstd::Impl<rstd::convert::From<curl::CURLcode>, ncrequest::Error> {
    static auto from(curl::CURLcode e) -> ncrequest::Error {
        return ncrequest::Error::Curl(e);
    };
};

template<>
struct rstd::Impl<rstd::convert::From<rstd::io::error::Error>, ncrequest::Error> {
    static auto from(rstd::io::error::Error e) -> ncrequest::Error {
        return ncrequest::Error::Io(rstd::move(e));
    };
};
