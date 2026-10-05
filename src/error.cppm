module;
#include <rstd/enum.hpp>

export module ncrequest:error;
export import rstd;
export import rstd.error;
#if ! defined(LITO_FEAT_QT)
export import ncrequest.curl;
#endif

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CStr;
using IoError = rstd::io::error::Error;
using rstd::error::ErrorRef;
using rstd::fmt::Debug;
using rstd::fmt::Display;

namespace ncrequest
{

export enum class ProtocolError {
    InvalidStatusLine,
    InvalidHeaderLine,
    HeaderTooLarge,
    BodyTooLarge,
    BodyLengthMismatch,
    UnexpectedEof,
    InvalidUtf8,
    InvalidRedirect,
    RedirectLimitExceeded,
    RedirectOriginChanged,
    RedirectBodyNotReplayable,
};

export enum class ErrorKind {
    Client,
    Io,
    Protocol,
    Unsupported,
    Canceled,
    InvalidState,
    Timeout,
};

export enum class ClientBackend {
    QtNetwork,
    Curl,
    CurlMulti,
    CurlShare,
};

export struct ClientError {
    ClientBackend backend;
    i32           code;
    String        message;
};

export struct Error {
    RSTD_ENUM_DEFAULT(Error, (InvalidState, "uncategorized ncrequest error"),
                      (Client, (ClientError error;)), (Io, (IoError error;)),
                      (Protocol, (ProtocolError kind; const char* msg;)),
                      (Unsupported, (const char* msg;)), (Canceled), (Timeout),
                      (InvalidState, (const char* msg;)))

    auto kind() const noexcept -> ErrorKind {
        switch (tag()) {
        case Tag::Client: return ErrorKind::Client;
        case Tag::Io: return ErrorKind::Io;
        case Tag::Protocol: return ErrorKind::Protocol;
        case Tag::Unsupported: return ErrorKind::Unsupported;
        case Tag::Canceled: return ErrorKind::Canceled;
        case Tag::Timeout: return ErrorKind::Timeout;
        case Tag::InvalidState: return ErrorKind::InvalidState;
        }
        return ErrorKind::InvalidState;
    }

    auto clone() const -> Error {
        switch (tag()) {
        case Tag::Client: {
            const auto& value = as_Client().error;
            return Client(ClientError { value.backend, value.code, value.message.clone() });
        }
        case Tag::Io: return Io(as_Io().error);
        case Tag::Protocol: return Protocol(as_Protocol().kind, as_Protocol().msg);
        case Tag::Unsupported: return Unsupported(as_Unsupported().msg);
        case Tag::Canceled: return Canceled();
        case Tag::InvalidState: return InvalidState(as_InvalidState().msg);
        case Tag::Timeout: return Timeout();
        }
        return InvalidState("unknown error tag");
    }
};

export template<typename T>
using Result = rstd::Result<T, Error>;

constexpr auto protocol_error_message(ProtocolError kind) noexcept -> const char* {
    switch (kind) {
    case ProtocolError::InvalidStatusLine: return "invalid HTTP status line";
    case ProtocolError::InvalidHeaderLine: return "invalid HTTP header line";
    case ProtocolError::HeaderTooLarge: return "HTTP header too large";
    case ProtocolError::BodyTooLarge: return "HTTP body too large";
    case ProtocolError::BodyLengthMismatch:
        return "request body length does not match its declared size";
    case ProtocolError::UnexpectedEof: return "unexpected EOF";
    case ProtocolError::InvalidUtf8: return "response text is not UTF-8";
    case ProtocolError::InvalidRedirect: return "invalid redirect location";
    case ProtocolError::RedirectLimitExceeded: return "redirect limit exceeded";
    case ProtocolError::RedirectOriginChanged: return "redirect changes HTTP origin";
    case ProtocolError::RedirectBodyNotReplayable: return "redirect body cannot be replayed";
    }
    return "protocol error";
}

} // namespace ncrequest

template<>
struct rstd::Impl<Display, ncrequest::ClientError> : rstd::ImplBase<ncrequest::ClientError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        auto& message = this->self().message;
        return as<Display>(message).fmt(f);
    }
};

template<>
struct rstd::Impl<Debug, ncrequest::ClientError> : rstd::ImplBase<ncrequest::ClientError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<Display>(this->self()).fmt(f); }
};

template<>
struct rstd::Impl<rstd::error::Error, ncrequest::ClientError>
    : DefaultInImpl<rstd::error::Error, ncrequest::ClientError> {};

template<>
struct rstd::Impl<Display, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        auto& e = this->self();
        switch (e.tag()) {
        case ncrequest::Error::Tag::Client: {
            constexpr char message[] = "client request failed";
            return f.write_raw(message, sizeof(message) - 1);
        }
        case ncrequest::Error::Tag::Io: {
            constexpr char message[] = "I/O request failed";
            return f.write_raw(message, sizeof(message) - 1);
        }
        case ncrequest::Error::Tag::Protocol: {
            auto& payload = e.as_Protocol();
            auto* msg = payload.msg != nullptr ? payload.msg
                                               : ncrequest::protocol_error_message(payload.kind);
            return f.write_raw(msg, rstd::strlen(msg));
        }
        case ncrequest::Error::Tag::Unsupported: {
            auto* msg = e.as_Unsupported().msg;
            if (msg == nullptr) msg = "unsupported ncrequest capability";
            return f.write_raw(msg, rstd::strlen(msg));
        }
        case ncrequest::Error::Tag::Canceled: {
            constexpr auto msg = "operation canceled"_str;
            return f.write_str(msg);
        }
        case ncrequest::Error::Tag::Timeout:
            return f.write_str("request total timeout exceeded"_str);
        case ncrequest::Error::Tag::InvalidState: {
            auto* msg = e.as_InvalidState().msg;
            if (msg == nullptr) msg = "invalid ncrequest state";
            return f.write_raw(msg, rstd::strlen(msg));
        }
        }
        return false;
    }
};

template<>
struct rstd::Impl<Debug, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<Display>(this->self()).fmt(f); }
};

template<>
struct rstd::Impl<rstd::error::Error, ncrequest::Error> : rstd::ImplBase<ncrequest::Error> {
    auto source() const noexcept -> Option<ErrorRef> {
        auto& error = this->self();
        switch (error.tag()) {
        case ncrequest::Error::Tag::Client:
            return Some(dyn<rstd::error::Error>::from_ref(error.as_Client().error));
        case ncrequest::Error::Tag::Io:
            return Some(dyn<rstd::error::Error>::from_ref(error.as_Io().error));
        default: return None();
        }
    }
};

static_assert(Impled<ncrequest::ClientError, rstd::error::Error>);
static_assert(Impled<ncrequest::Error, rstd::error::Error>);

template<>
struct rstd::Impl<From<ncrequest::ClientError>, ncrequest::Error> {
    static auto from(ncrequest::ClientError error) -> ncrequest::Error {
        return ncrequest::Error::Client(rstd::move(error));
    }
};

#if ! defined(LITO_FEAT_QT)
template<>
struct rstd::Impl<From<curl::CURLcode>, ncrequest::Error> {
    static auto from(curl::CURLcode e) -> ncrequest::Error {
        if (e == curl::CURLcode::CURLE_OPERATION_TIMEDOUT) return ncrequest::Error::Timeout();
        auto* message = curl::curl_easy_strerror(e);
        return rstd::into(ncrequest::ClientError {
            .backend = ncrequest::ClientBackend::Curl,
            .code    = static_cast<i32>(e),
            .message =
                String::make(CStr::from_ptr(message != nullptr ? message : "curl client error")
                                 .to_str()
                                 .unwrap()),
        });
    };
};
#endif

#if ! defined(LITO_FEAT_QT)
template<>
struct rstd::Impl<From<ncrequest::CurlMultiError>, ncrequest::Error> {
    static auto from(ncrequest::CurlMultiError error) -> ncrequest::Error {
        if (error.is_Easy()) return rstd::into<ncrequest::Error>(error.as_Easy().code);
        auto shared  = error.is_Share();
        auto code    = shared ? static_cast<i32>(error.as_Share().code)
                              : static_cast<i32>(error.as_Multi().code);
        auto message = shared ? curl::curl_share_strerror(error.as_Share().code)
                              : curl::curl_multi_strerror(error.as_Multi().code);
        return ncrequest::Error::Client(ncrequest::ClientError {
            shared ? ncrequest::ClientBackend::CurlShare : ncrequest::ClientBackend::CurlMulti,
            code,
            String::make(CStr::from_ptr(message ? message : "curl initialization error")
                             .to_str()
                             .unwrap()) });
    }
};
#endif

template<>
struct rstd::Impl<From<IoError>, ncrequest::Error> {
    static auto from(IoError e) -> ncrequest::Error { return ncrequest::Error::Io(rstd::move(e)); };
};
