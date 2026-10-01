export module ncrequest:request;
export import lihttpto;
export import :error;
export import :options;
export import :request_body;
export import :session_share;
export import rstd;

using namespace rstd::prelude;
using rstd::bytes::Bytes;

namespace ncrequest
{

export auto global_init() -> Result<empty>;

export class Request {
public:
    Request() noexcept;
    explicit Request(lihttpto::Url url) noexcept;
    Request(Request&&) noexcept;
    ~Request() noexcept;
    Request& operator=(Request&&) noexcept;

    [[nodiscard]]
    static auto from_url(ref<str>) -> rstd::Result<Request, lihttpto::UrlError>;

    auto method() const -> const lihttpto::Method&;
    auto set_method(lihttpto::Method method) -> Request&;
    auto try_set_method(ref<str> method) -> rstd::Result<empty, lihttpto::HttpParseError>;
    auto body() const -> const RequestBody&;
    auto set_body(RequestBody body) -> Request&;
    auto set_body(Bytes body) -> Request&;
    auto clear_body() -> Request&;
    auto validate() const -> Result<empty>;
    auto options() const -> const RequestOptions& { return m_options; }
    auto set_options(RequestOptions options) -> Request& {
        m_options = rstd::move(options);
        return *this;
    }

    auto url() const -> ref<str>;
    auto url_info() const -> const lihttpto::Url&;
    auto try_set_url(ref<str>) -> rstd::Result<empty, lihttpto::UrlError>;

    auto header() const -> const lihttpto::Headers&;
    auto header(ref<str> name) const -> Option<ref<lihttpto::HeaderValue>>;
    auto update_header(const lihttpto::Headers&) -> Request&;
    auto try_set_header(ref<str> name, ref<str> value)
        -> rstd::Result<empty, lihttpto::HeaderError>;
    auto remove_header(ref<str> name) -> Request&;
    auto try_clone() const -> Result<Request>;

private:
    lihttpto::Method  m_method;
    RequestBody       m_body;
    lihttpto::Url     m_url;
    lihttpto::Headers m_header;
    RequestOptions    m_options;
};

export class PreparedRequest {
    Request          request_;
    EffectiveOptions options_;
    PreparedRequest(Request request, EffectiveOptions options)
        : request_(rstd::move(request)), options_(rstd::move(options)) {}

public:
    static auto prepare(Request request, const SessionOptions& session) -> Result<PreparedRequest> {
        auto valid = request.validate();
        if (valid.is_err()) return Err(rstd::move(valid).unwrap_err());
        auto options = EffectiveOptions::resolve(session, request.options());
        if (options.is_err()) return Err(rstd::move(options).unwrap_err());
        return Ok(PreparedRequest(rstd::move(request), rstd::move(options).unwrap()));
    }
    auto request() const -> const Request& { return request_; }
    auto options() const -> const EffectiveOptions& { return options_; }
};

} // namespace ncrequest
