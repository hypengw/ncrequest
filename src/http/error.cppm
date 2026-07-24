module;
#include <rstd/enum.hpp>

export module ncrequest:http_error;
export import rstd;

namespace ncrequest::http
{

using namespace rstd::prelude;

export struct UrlErrorKind {
    RSTD_ENUM(UrlErrorKind, (InvalidSyntax), (InvalidCharacter), (InvalidPercentEncoding),
              (InvalidIpAddress), (InvalidPort), (UnexpectedEnd), (MissingScheme),
              (UnsupportedScheme), (MissingAuthority), (MissingHost))
};

export struct HeaderErrorKind {
    RSTD_ENUM(HeaderErrorKind, (InvalidName), (InvalidValue), (InvalidLineBreak), (InvalidSyntax))
};

export struct QueryErrorKind {
    RSTD_ENUM(QueryErrorKind, (InvalidPercentEncoding), (InvalidUtf8))
};

export struct CookieErrorKind {
    RSTD_ENUM(CookieErrorKind, (EmptyName), (InvalidName), (InvalidValue), (InvalidAttribute),
              (InvalidSyntax))
};

export struct HttpParseErrorKind {
    RSTD_ENUM(HttpParseErrorKind, (InvalidStartLine), (InvalidHeaderLine), (InvalidSyntax),
              (HeaderTooLarge), (UnexpectedEof))
};

export class UrlError {
public:
    constexpr UrlError(UrlErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const UrlErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    UrlErrorKind kind_;
    usize        offset_;
};

export class HeaderError {
public:
    constexpr HeaderError(HeaderErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const HeaderErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    HeaderErrorKind kind_;
    usize           offset_;
};

export class HttpParseError {
public:
    constexpr HttpParseError(HttpParseErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const HttpParseErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    HttpParseErrorKind kind_;
    usize              offset_;
};

export class QueryError {
public:
    constexpr QueryError(QueryErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const QueryErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    QueryErrorKind kind_;
    usize          offset_;
};

export class CookieError {
public:
    constexpr CookieError(CookieErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const CookieErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    CookieErrorKind kind_;
    usize           offset_;
};

namespace detail
{

inline auto message(const UrlErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case UrlErrorKind::Tag::InvalidSyntax: return "invalid URI reference";
    case UrlErrorKind::Tag::InvalidCharacter: return "invalid character in URI reference";
    case UrlErrorKind::Tag::InvalidPercentEncoding:
        return "invalid percent encoding in URI reference";
    case UrlErrorKind::Tag::InvalidIpAddress: return "invalid IP address in URI reference";
    case UrlErrorKind::Tag::InvalidPort: return "invalid port in URI reference";
    case UrlErrorKind::Tag::UnexpectedEnd: return "unexpected end of URI reference";
    case UrlErrorKind::Tag::MissingScheme: return "HTTP URL is missing a scheme";
    case UrlErrorKind::Tag::UnsupportedScheme: return "HTTP URL has an unsupported scheme";
    case UrlErrorKind::Tag::MissingAuthority: return "HTTP URL is missing an authority";
    case UrlErrorKind::Tag::MissingHost: return "HTTP URL is missing a host";
    }
    rstd::unreachable();
}

inline auto message(const HeaderErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case HeaderErrorKind::Tag::InvalidName: return "invalid HTTP field name";
    case HeaderErrorKind::Tag::InvalidValue: return "invalid HTTP field value";
    case HeaderErrorKind::Tag::InvalidLineBreak: return "line break in HTTP field value";
    case HeaderErrorKind::Tag::InvalidSyntax: return "invalid HTTP field syntax";
    }
    rstd::unreachable();
}

inline auto message(const HttpParseErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case HttpParseErrorKind::Tag::InvalidStartLine: return "invalid HTTP start line";
    case HttpParseErrorKind::Tag::InvalidHeaderLine: return "invalid HTTP field line";
    case HttpParseErrorKind::Tag::InvalidSyntax: return "invalid HTTP message syntax";
    case HttpParseErrorKind::Tag::HeaderTooLarge: return "HTTP field section is too large";
    case HttpParseErrorKind::Tag::UnexpectedEof: return "unexpected end of HTTP message head";
    }
    rstd::unreachable();
}

inline auto message(const QueryErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case QueryErrorKind::Tag::InvalidPercentEncoding: return "invalid percent encoding in query";
    case QueryErrorKind::Tag::InvalidUtf8: return "invalid UTF-8 in query";
    }
    rstd::unreachable();
}

inline auto message(const CookieErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case CookieErrorKind::Tag::EmptyName: return "empty cookie name";
    case CookieErrorKind::Tag::InvalidName: return "invalid cookie name";
    case CookieErrorKind::Tag::InvalidValue: return "invalid cookie value";
    case CookieErrorKind::Tag::InvalidAttribute: return "invalid cookie attribute";
    case CookieErrorKind::Tag::InvalidSyntax: return "invalid cookie syntax";
    }
    rstd::unreachable();
}

} // namespace detail

} // namespace ncrequest::http

namespace rstd
{

export template<>
struct Impl<fmt::Display, ncrequest::http::UrlError> : ImplBase<ncrequest::http::UrlError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = ncrequest::http::detail::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::HeaderError> : ImplBase<ncrequest::http::HeaderError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = ncrequest::http::detail::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::HttpParseError>
    : ImplBase<ncrequest::http::HttpParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = ncrequest::http::detail::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::QueryError> : ImplBase<ncrequest::http::QueryError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = ncrequest::http::detail::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::CookieError> : ImplBase<ncrequest::http::CookieError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = ncrequest::http::detail::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Debug, ncrequest::http::UrlError> : ImplBase<ncrequest::http::UrlError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, ncrequest::http::HeaderError> : ImplBase<ncrequest::http::HeaderError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, ncrequest::http::HttpParseError>
    : ImplBase<ncrequest::http::HttpParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, ncrequest::http::QueryError> : ImplBase<ncrequest::http::QueryError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, ncrequest::http::CookieError> : ImplBase<ncrequest::http::CookieError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<error::Error, ncrequest::http::UrlError>
    : DefaultInImpl<error::Error, ncrequest::http::UrlError> {};

export template<>
struct Impl<error::Error, ncrequest::http::HeaderError>
    : DefaultInImpl<error::Error, ncrequest::http::HeaderError> {};

export template<>
struct Impl<error::Error, ncrequest::http::HttpParseError>
    : DefaultInImpl<error::Error, ncrequest::http::HttpParseError> {};

export template<>
struct Impl<error::Error, ncrequest::http::QueryError>
    : DefaultInImpl<error::Error, ncrequest::http::QueryError> {};

export template<>
struct Impl<error::Error, ncrequest::http::CookieError>
    : DefaultInImpl<error::Error, ncrequest::http::CookieError> {};

} // namespace rstd

namespace ncrequest::http
{

static_assert(Impled<UrlError, rstd::error::Error>);
static_assert(Impled<HeaderError, rstd::error::Error>);
static_assert(Impled<HttpParseError, rstd::error::Error>);
static_assert(Impled<QueryError, rstd::error::Error>);
static_assert(Impled<CookieError, rstd::error::Error>);

} // namespace ncrequest::http
