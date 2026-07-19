export module ncrequest:http_url;
export import :http_error;
export import rstd;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;

export class Url : public DefaultInClass<Url, Clone> {
    struct Component {
        usize offset {};
        usize size {};
        bool  present = false;
    };

public:
    Url() noexcept                         = default;
    Url(Url&&) noexcept                    = default;
    auto operator=(Url&&) noexcept -> Url& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<Url, UrlError>;

    [[nodiscard]]
    static auto parse_http(ref<str> input) -> Result<Url, UrlError>;

    [[nodiscard]]
    auto as_ref() const noexcept -> ref<str>;

    [[nodiscard]]
    auto scheme() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto authority() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto userinfo() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto host() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto port() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto path() const noexcept -> ref<str>;

    [[nodiscard]]
    auto query() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto fragment() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto request_target() const -> String;

    [[nodiscard]]
    auto resolve(const Url& reference) const -> Result<Url, UrlError>;

    [[nodiscard]]
    auto clone() const -> Url;

private:
    Url(String source, Component scheme, Component authority, Component userinfo, Component host,
        Component port, Component path, Component query, Component fragment) noexcept;

    [[nodiscard]]
    auto component(Component value) const noexcept -> Option<ref<str>>;

    String    source_;
    Component scheme_;
    Component authority_;
    Component userinfo_;
    Component host_;
    Component port_;
    Component path_ { .present = true };
    Component query_;
    Component fragment_;
};

static_assert(Impled<Url, Clone>);
static_assert(Impled<Url, rstd::convert::AsRef<str>>);

} // namespace ncrequest::http

namespace rstd
{

export template<>
struct Impl<str_::FromStr, ncrequest::http::Url> : ImplBase<ncrequest::http::Url> {
    using Err = ncrequest::http::UrlError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::Url, Err> {
        return ncrequest::http::Url::parse(input);
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::Url> : ImplBase<ncrequest::http::Url> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = this->self().as_ref();
        return formatter.write_raw(value.data(), value.size().to_primitive());
    }
};

} // namespace rstd

namespace ncrequest::http
{

static_assert(Impled<Url, rstd::str_::FromStr>);
static_assert(Impled<Url, rstd::fmt::Display>);

} // namespace ncrequest::http
