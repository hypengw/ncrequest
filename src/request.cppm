export module ncrequest:request;
export import :options;
export import :request_body;
export import rstd.json;
using namespace rstd::prelude;
using namespace rstd::literals;
export namespace ncrequest
{
namespace method
{
inline constexpr auto get = "GET"_str, post = "POST"_str, put = "PUT"_str, head = "HEAD"_str,
                      delete_ = "DELETE"_str, patch = "PATCH"_str, options = "OPTIONS"_str;
}
namespace header
{
inline constexpr auto accept = "Accept"_str, content_type = "Content-Type"_str,
                      authorization = "Authorization"_str, user_agent = "User-Agent"_str;
}
namespace media_type
{
inline constexpr auto json = "application/json"_str, form = "application/x-www-form-urlencoded"_str,
                      text = "text/plain; charset=utf-8"_str;
}
class Session;
class Request {
    friend class Session;
    lihttpto::RequestParts parts_;
    RequestBody            body_;
    RequestOptions         options_;

public:
    Request();
    explicit Request(lihttpto::Url url);
    explicit Request(lifetchto::http::ExecutionRequest message);
    Request(Request&&) noexcept                           = default;
    Request(const Request&)                               = delete;
    auto        operator=(Request&&) noexcept -> Request& = default;
    static auto from_url(ref<str>) -> rstd::Result<Request, lihttpto::UrlError>;
    auto        parts() const -> const lihttpto::RequestParts& { return parts_; }
    auto        method() const -> const lihttpto::Method& { return parts_.method; }
    auto        set_method(lihttpto::Method method) -> Request&;
    auto        try_set_method(ref<str>) -> rstd::Result<empty, lihttpto::HttpParseError>;
    auto        url() const -> ref<str> { return parts_.url.as_ref(); }
    auto        url_info() const -> const lihttpto::Url& { return parts_.url; }
    auto        try_set_url(ref<str>) -> rstd::Result<empty, lihttpto::UrlError>;
    auto        header() const -> const lihttpto::Headers& { return parts_.headers; }
    auto        header(ref<str> name) const -> Option<ref<lihttpto::HeaderValue>> {
        return parts_.headers.get(name);
    }
    auto update_header(const lihttpto::Headers&) -> Request&;
    auto try_set_header(ref<str>, ref<str>) -> rstd::Result<empty, lihttpto::HeaderError>;
    auto try_set_header(ref<str>, slice<u8>) -> rstd::Result<empty, lihttpto::HeaderError>;
    auto add_header(ref<str>, slice<u8>) -> rstd::Result<empty, lihttpto::HeaderError>;
    auto remove_header(ref<str> name) -> Request&;
    auto body() const -> const RequestBody& { return body_; }
    auto set_body(RequestBody body) -> Request& {
        body_ = rstd::move(body);
        return *this;
    }
    auto set_body(Bytes body) -> Request& { return set_body(RequestBody::bytes(rstd::move(body))); }
    auto clear_body() -> Request& { return set_body(RequestBody::empty()); }
    auto options() -> RequestOptions& { return options_; }
    auto options() const -> const RequestOptions& { return options_; }
    auto set_options(RequestOptions options) -> Request& {
        options_ = rstd::move(options);
        return *this;
    }
    auto text(ref<str> value, ref<str> type = media_type::text)
        -> rstd::Result<empty, lihttpto::HeaderError>;
    auto json(const rstd::json::Value&) -> rstd::Result<empty, lihttpto::HeaderError>;
    auto form(const lihttpto::QueryParams&) -> rstd::Result<empty, lihttpto::HeaderError>;
    auto into_message() && -> lifetchto::http::ExecutionRequest {
        return { rstd::move(parts_), rstd::move(body_) };
    }
    void request_close() const { body_.request_close(); }
    auto close() const -> coro<lifetchto::Result<empty>> { return body_.close(); }
    auto wait_closed() const -> coro<lifetchto::CleanupReport> { return body_.wait_closed(); }
    auto take_close_result() const -> Option<lifetchto::Result<empty>> {
        return body_.take_close_result();
    }
};
} // namespace ncrequest
