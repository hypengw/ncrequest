module;
#include <rstd/enum.hpp>

export module ncrequest:http_message;
export import :http_header;
export import rstd;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

#define NCREQUEST_OPERATION_VARIANTS(V) \
    V(Get)                              \
    V(Post)                             \
    V(Delete)                           \
    V(Head)

export struct Operation {
    RSTD_TAG_ENUM_BODY(Operation, NCREQUEST_OPERATION_VARIANTS)
};

#undef NCREQUEST_OPERATION_VARIANTS

export class Method : public DefaultInClass<Method, Clone> {
public:
    Method(Method&&) noexcept = default;
    auto operator=(Method&&) noexcept -> Method& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<Method, HttpParseError>;

    [[nodiscard]]
    auto as_ref() const noexcept -> ref<str>;

    [[nodiscard]]
    auto clone() const -> Method;

private:
    explicit Method(String value) noexcept;

    String value_;
};

export class Version {
public:
    constexpr Version(u8 major, u8 minor) noexcept: major_(major), minor_(minor) {}

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<Version, HttpParseError>;

    [[nodiscard]]
    constexpr auto major() const noexcept -> u8 {
        return major_;
    }

    [[nodiscard]]
    constexpr auto minor() const noexcept -> u8 {
        return minor_;
    }

private:
    u8 major_;
    u8 minor_;
};

export class StatusCode {
public:
    [[nodiscard]]
    static auto make(u16 value) -> Result<StatusCode, HttpParseError>;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<StatusCode, HttpParseError>;

    [[nodiscard]]
    constexpr auto value() const noexcept -> u16 {
        return value_;
    }

private:
    explicit constexpr StatusCode(u16 value) noexcept: value_(value) {}

    u16 value_;
};

export class RequestLine : public DefaultInClass<RequestLine, Clone> {
public:
    RequestLine(Method method, String target, Version version) noexcept;

    [[nodiscard]]
    auto method() const noexcept -> const Method&;

    [[nodiscard]]
    auto target() const noexcept -> ref<str>;

    [[nodiscard]]
    auto version() const noexcept -> Version;

    [[nodiscard]]
    auto clone() const -> RequestLine;

private:
    Method  method_;
    String  target_;
    Version version_;
};

export class StatusLine : public DefaultInClass<StatusLine, Clone> {
public:
    StatusLine(Option<Version> version, StatusCode status,
               Option<HeaderValue> reason) noexcept;

    [[nodiscard]]
    auto version() const noexcept -> Option<Version>;

    [[nodiscard]]
    auto status() const noexcept -> StatusCode;

    [[nodiscard]]
    auto reason() const noexcept -> Option<ref<HeaderValue>>;

    [[nodiscard]]
    auto clone() const -> StatusLine;

private:
    Option<Version>     version_;
    StatusCode          status_;
    Option<HeaderValue> reason_;
};

#define NCREQUEST_START_LINE_VARIANTS(V) \
    V(Request, (RequestLine value;))      \
    V(Response, (StatusLine value;))

export class StartLine : public DefaultInClass<StartLine, Clone> {
    RSTD_ENUM_BODY(StartLine, NCREQUEST_START_LINE_VARIANTS)

    [[nodiscard]]
    auto clone() const -> StartLine;
};

#undef NCREQUEST_START_LINE_VARIANTS

export class MessageHead : public DefaultInClass<MessageHead, Clone> {
public:
    MessageHead(StartLine start, Header headers) noexcept;

    [[nodiscard]]
    static auto parse(slice<u8> input) -> Result<MessageHead, HttpParseError>;

    [[nodiscard]]
    auto start() const noexcept -> const StartLine&;

    [[nodiscard]]
    auto headers() const noexcept -> const Header&;

    [[nodiscard]]
    auto status_code() const noexcept -> Option<u16>;

    [[nodiscard]]
    auto has_field(ref<str> name) const noexcept -> bool;

    [[nodiscard]]
    auto clone() const -> MessageHead;

private:
    StartLine start_;
    Header    headers_;
};

#define NCREQUEST_HTTP1_HEAD_EVENT_VARIANTS(V) \
    V(NeedMore, ())                             \
    V(Complete, (MessageHead head; usize consumed;))

export class Http1HeadEvent {
    RSTD_ENUM_BODY(Http1HeadEvent, NCREQUEST_HTTP1_HEAD_EVENT_VARIANTS)
};

#undef NCREQUEST_HTTP1_HEAD_EVENT_VARIANTS

export class Http1HeadParser {
public:
    static constexpr usize MaxHeaderBytes = 64 * 1024;

    Http1HeadParser() noexcept = default;
    Http1HeadParser(Http1HeadParser&&) noexcept = default;
    auto operator=(Http1HeadParser&&) noexcept -> Http1HeadParser& = default;

    [[nodiscard]]
    auto push(slice<u8> input) -> Result<Http1HeadEvent, HttpParseError>;

    [[nodiscard]]
    auto finish() -> Result<MessageHead, HttpParseError>;

private:
    Vec<u8>           buffer_;
    usize             line_start_ = 0;
    usize             scan_       = 0;
    Option<StartLine> start_;
    Header            headers_;
    bool              complete_ = false;
};

#define NCREQUEST_HTTP1_FIELD_SECTION_EVENT_VARIANTS(V) \
    V(NeedMore, ())                                      \
    V(Complete, (Header fields; usize consumed;))

export class Http1FieldSectionEvent {
    RSTD_ENUM_BODY(Http1FieldSectionEvent, NCREQUEST_HTTP1_FIELD_SECTION_EVENT_VARIANTS)
};

#undef NCREQUEST_HTTP1_FIELD_SECTION_EVENT_VARIANTS

export class Http1FieldSectionParser {
public:
    static constexpr usize MaxHeaderBytes = Http1HeadParser::MaxHeaderBytes;

    Http1FieldSectionParser() noexcept = default;
    Http1FieldSectionParser(Http1FieldSectionParser&&) noexcept = default;
    auto operator=(Http1FieldSectionParser&&) noexcept
        -> Http1FieldSectionParser& = default;

    [[nodiscard]]
    auto push(slice<u8> input) -> Result<Http1FieldSectionEvent, HttpParseError>;

    [[nodiscard]]
    auto finish() -> Result<Header, HttpParseError>;

private:
    Vec<u8> buffer_;
    usize   line_start_ = 0;
    usize   scan_       = 0;
    Header  fields_;
    bool    complete_ = false;
};

} // namespace ncrequest::http

namespace rstd
{

export template<>
struct Impl<str_::FromStr, ncrequest::http::Method> : ImplBase<ncrequest::http::Method> {
    using Err = ncrequest::http::HttpParseError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::Method, Err> {
        return ncrequest::http::Method::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, ncrequest::http::Version> : ImplBase<ncrequest::http::Version> {
    using Err = ncrequest::http::HttpParseError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::Version, Err> {
        return ncrequest::http::Version::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, ncrequest::http::StatusCode>
    : ImplBase<ncrequest::http::StatusCode> {
    using Err = ncrequest::http::HttpParseError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::StatusCode, Err> {
        return ncrequest::http::StatusCode::parse(input);
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::Method> : ImplBase<ncrequest::http::Method> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = this->self().as_ref();
        return formatter.write_raw(value.data(), value.size());
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::Version> : ImplBase<ncrequest::http::Version> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const u8 value[] = { 'H', 'T', 'T', 'P', '/',
                             static_cast<u8>('0' + this->self().major()), '.',
                             static_cast<u8>('0' + this->self().minor()) };
        return formatter.write_raw(value, 8);
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::StatusCode>
    : ImplBase<ncrequest::http::StatusCode> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = this->self().value();
        const u8 bytes[] = { static_cast<u8>('0' + value / 100),
                             static_cast<u8>('0' + value / 10 % 10),
                             static_cast<u8>('0' + value % 10) };
        return formatter.write_raw(bytes, 3);
    }
};

} // namespace rstd

namespace ncrequest::http
{

static_assert(Impled<Method, Clone>);
static_assert(Impled<Method, AsRef<str>>);
static_assert(Impled<Method, rstd::str_::FromStr>);
static_assert(Impled<Method, rstd::fmt::Display>);
static_assert(Impled<Version, Clone>);
static_assert(Impled<Version, rstd::str_::FromStr>);
static_assert(Impled<Version, rstd::fmt::Display>);
static_assert(Impled<StatusCode, Clone>);
static_assert(Impled<StatusCode, rstd::str_::FromStr>);
static_assert(Impled<StatusCode, rstd::fmt::Display>);
static_assert(Impled<RequestLine, Clone>);
static_assert(Impled<StatusLine, Clone>);
static_assert(Impled<StartLine, Clone>);
static_assert(Impled<MessageHead, Clone>);

} // namespace ncrequest::http
