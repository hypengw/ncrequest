module ncrequest;
import :request;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest
{
Request::Request(): parts_ { {}, lihttpto::Method::parse(method::get).unwrap(), {} } {}
Request::Request(lihttpto::Url url)
    : parts_ { rstd::move(url), lihttpto::Method::parse(method::get).unwrap(), {} } {}
Request::Request(lifetchto::http::ExecutionRequest message)
    : parts_(rstd::move(message.parts)), body_(rstd::move(message.body)) {}
auto Request::from_url(ref<str> value) -> rstd::Result<Request, lihttpto::UrlError> {
    auto url = lihttpto::Url::parse_http(value);
    if (url.is_err()) return Err(rstd::move(url).unwrap_err());
    return Ok(Request(rstd::move(url).unwrap()));
}
auto Request::set_method(lihttpto::Method value) -> Request& {
    parts_.method = rstd::move(value);
    return *this;
}
auto Request::try_set_method(ref<str> value) -> rstd::Result<empty, lihttpto::HttpParseError> {
    auto method = lihttpto::Method::parse(value);
    if (method.is_err()) return Err(rstd::move(method).unwrap_err());
    parts_.method = rstd::move(method).unwrap();
    return Ok(empty {});
}
auto Request::try_set_url(ref<str> value) -> rstd::Result<empty, lihttpto::UrlError> {
    auto url = lihttpto::Url::parse_http(value);
    if (url.is_err()) return Err(rstd::move(url).unwrap_err());
    parts_.url = rstd::move(url).unwrap();
    return Ok(empty {});
}
auto Request::update_header(const lihttpto::Headers& headers) -> Request& {
    for (const auto& field : headers) parts_.headers.remove(field.name.as_str());
    for (const auto& field : headers) parts_.headers.push(field.clone());
    return *this;
}
auto Request::try_set_header(ref<str> name, ref<str> value)
    -> rstd::Result<empty, lihttpto::HeaderError> {
    return parts_.headers.set(name, value);
}
auto Request::try_set_header(ref<str> name, slice<u8> value)
    -> rstd::Result<empty, lihttpto::HeaderError> {
    auto parsed = lihttpto::HeaderValue::make(value);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    return parts_.headers.set(name, rstd::move(parsed).unwrap());
}
auto Request::add_header(ref<str> name, slice<u8> value)
    -> rstd::Result<empty, lihttpto::HeaderError> {
    auto parsed = lihttpto::Header::make(name, value);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    parts_.headers.push(rstd::move(parsed).unwrap());
    return Ok(empty {});
}
auto Request::remove_header(ref<str> name) -> Request& {
    parts_.headers.remove(name);
    return *this;
}
auto Request::text(ref<str> value, ref<str> type) -> rstd::Result<empty, lihttpto::HeaderError> {
    auto set = try_set_header(header::content_type, type);
    if (set.is_err()) return set;
    set_body(Bytes::copy_from_slice(value.as_bytes()));
    return Ok(empty {});
}
auto Request::json(const rstd::json::Value& value) -> rstd::Result<empty, lihttpto::HeaderError> {
    auto encoded = rstd::json::to_string(value);
    return text(encoded.as_str(), media_type::json);
}
auto Request::form(const lihttpto::QueryParams& value)
    -> rstd::Result<empty, lihttpto::HeaderError> {
    auto encoded = value.encode_form();
    return text(encoded.as_str(), media_type::form);
}
} // namespace ncrequest
