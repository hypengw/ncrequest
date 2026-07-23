export module ncrequest:http_header;
export import :http_error;
export import rstd;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

export class HeaderName : public DefaultInClass<HeaderName, Clone> {
public:
    HeaderName(HeaderName&&) noexcept                    = default;
    auto operator=(HeaderName&&) noexcept -> HeaderName& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<HeaderName, HeaderError>;

    [[nodiscard]]
    auto as_ref() const noexcept -> ref<str>;

    [[nodiscard]]
    auto equals(ref<str> other) const noexcept -> bool;

    [[nodiscard]]
    auto clone() const -> HeaderName;

private:
    explicit HeaderName(String value) noexcept;

    String value_;
};

export class HeaderValue : public DefaultInClass<HeaderValue, Clone> {
public:
    HeaderValue(HeaderValue&&) noexcept                    = default;
    auto operator=(HeaderValue&&) noexcept -> HeaderValue& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<HeaderValue, HeaderError>;

    [[nodiscard]]
    static auto from_bytes(slice<u8> input) -> Result<HeaderValue, HeaderError>;

    [[nodiscard]]
    auto as_bytes() const noexcept -> slice<u8>;

    [[nodiscard]]
    auto as_str() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto clone() const -> HeaderValue;

private:
    explicit HeaderValue(Vec<u8> value) noexcept;

    Vec<u8> value_;
};

export class HeaderField : public DefaultInClass<HeaderField, Clone> {
public:
    HeaderField(HeaderName name, HeaderValue value) noexcept;
    HeaderField(HeaderField&&) noexcept                    = default;
    auto operator=(HeaderField&&) noexcept -> HeaderField& = default;

    [[nodiscard]]
    auto name() const noexcept -> const HeaderName&;

    [[nodiscard]]
    auto value() const noexcept -> const HeaderValue&;

    [[nodiscard]]
    auto clone() const -> HeaderField;

private:
    HeaderName  name_;
    HeaderValue value_;
};

export class HeaderIter : public DefaultInClass<HeaderIter, Iterator> {
public:
    using Item = ref<HeaderField>;

    HeaderIter(const HeaderField* current, const HeaderField* end) noexcept;

    [[nodiscard]]
    auto next() noexcept -> Option<Item>;

private:
    const HeaderField* current_;
    const HeaderField* end_;
};

export class HeaderValues : public DefaultInClass<HeaderValues, Iterator> {
public:
    using Item = ref<HeaderValue>;

    HeaderValues(const HeaderField* current, const HeaderField* end,
                 const HeaderName* name) noexcept;

    [[nodiscard]]
    auto next() noexcept -> Option<Item>;

private:
    const HeaderField* current_;
    const HeaderField* end_;
    const HeaderName*  name_;
};

export class Header : public DefaultInClass<Header, Clone> {
public:
    Header() noexcept                            = default;
    Header(Header&&) noexcept                    = default;
    auto operator=(Header&&) noexcept -> Header& = default;

    [[nodiscard]]
    auto add(ref<str> name, HeaderValue value) -> Result<empty, HeaderError>;

    [[nodiscard]]
    auto add(ref<str> name, ref<str> value) -> Result<empty, HeaderError>;

    [[nodiscard]]
    auto set(ref<str> name, HeaderValue value) -> Result<empty, HeaderError>;

    [[nodiscard]]
    auto set(ref<str> name, ref<str> value) -> Result<empty, HeaderError>;

    void append(HeaderField field);

    [[nodiscard]]
    auto get(ref<str> name) const noexcept -> Option<ref<HeaderValue>>;

    [[nodiscard]]
    auto values(ref<str> name) const noexcept -> HeaderValues;

    [[nodiscard]]
    auto contains(ref<str> name) const noexcept -> bool;

    [[nodiscard]]
    auto has_field(ref<str> name) const noexcept -> bool;

    [[nodiscard]]
    auto remove(ref<str> name) noexcept -> usize;

    [[nodiscard]]
    auto len() const noexcept -> usize;

    [[nodiscard]]
    auto is_empty() const noexcept -> bool;

    [[nodiscard]]
    auto iter() const noexcept -> HeaderIter;

    [[nodiscard]]
    auto clone() const -> Header;

private:
    Vec<HeaderField> fields_;
};

} // namespace ncrequest::http

namespace rstd
{

export template<>
struct Impl<str_::FromStr, ncrequest::http::HeaderName> : ImplBase<ncrequest::http::HeaderName> {
    using Err = ncrequest::http::HeaderError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::HeaderName, Err> {
        return ncrequest::http::HeaderName::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, ncrequest::http::HeaderValue> : ImplBase<ncrequest::http::HeaderValue> {
    using Err = ncrequest::http::HeaderError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::HeaderValue, Err> {
        return ncrequest::http::HeaderValue::parse(input);
    }
};

} // namespace rstd

namespace ncrequest::http
{

static_assert(Impled<HeaderName, Clone>);
static_assert(Impled<HeaderName, AsRef<str>>);
static_assert(Impled<HeaderName, rstd::str_::FromStr>);
static_assert(Impled<HeaderValue, Clone>);
static_assert(Impled<HeaderValue, rstd::str_::FromStr>);
static_assert(Impled<HeaderField, Clone>);
static_assert(Impled<Header, Clone>);
static_assert(Impled<HeaderIter, Iterator>);
static_assert(Impled<HeaderValues, Iterator>);

} // namespace ncrequest::http
