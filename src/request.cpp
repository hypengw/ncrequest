module ncrequest;
import :request;

#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
import ncrequest.curl;
#endif

using namespace rstd::prelude;
using namespace ncrequest;
using namespace rstd::literals;
using rstd::bytes::Bytes;

auto ncrequest::global_init() -> Result<empty> {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    auto initialized = ncrequest::curl_init();
    if (initialized.is_err()) {
        return Err(rstd::into<Error>(rstd::move(initialized).unwrap_err()));
    }
    return Ok(empty {});
#else
    return Ok(empty {});
#endif
}

Request::Request() noexcept: m_method(lihttpto::Method::parse("GET"_str).unwrap()) {}
Request::Request(lihttpto::Url url) noexcept: Request() { m_url = rstd::move(url); }
Request::~Request() noexcept {}
Request::Request(Request&&) noexcept            = default;
Request& Request::operator=(Request&&) noexcept = default;

auto Request::from_url(ref<str> input) -> rstd::Result<Request, lihttpto::UrlError> {
    auto parsed = lihttpto::Url::parse_http(input);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    return Ok(Request { rstd::move(parsed).unwrap() });
}

auto Request::method() const -> const lihttpto::Method& { return m_method; }
auto Request::set_method(lihttpto::Method method) -> Request& {
    m_method = rstd::move(method);
    return *this;
}
auto Request::try_set_method(ref<str> method) -> rstd::Result<empty, lihttpto::HttpParseError> {
    auto parsed = lihttpto::Method::parse(method);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    m_method = rstd::move(parsed).unwrap();
    return Ok(empty {});
}
auto Request::body() const -> const RequestBody& { return m_body; }
auto Request::set_body(RequestBody body) -> Request& {
    m_body = rstd::move(body);
    return *this;
}
auto Request::set_body(Bytes body) -> Request& {
    return set_body(RequestBody::from_bytes(rstd::move(body)));
}
auto Request::clear_body() -> Request& {
    m_body = RequestBody {};
    return *this;
}
auto Request::validate() const -> Result<empty> {
    if (m_url.as_ref().size() == usize()) return Err(Error::InvalidState("request URL is empty"));
    auto scheme = m_url.scheme();
    if (scheme != Some("http"_str) && scheme != Some("https"_str))
        return Err(Error::Unsupported("request URL requires HTTP or HTTPS"));
    if (m_body.reader().is_some() && m_method.as_ref() != "POST"_str)
        return Err(Error::Unsupported("body reader requires POST"));
    if (m_method.as_ref() == "HEAD"_str && m_body.stream().is_some())
        return Err(Error::Unsupported("HEAD request cannot contain a body source"));
    if (m_method.as_ref() == "HEAD"_str && m_body.bytes().is_some() &&
        m_body.bytes()->size() != usize())
        return Err(Error::InvalidState("HEAD request cannot contain a body"));
    return Ok(empty {});
}

auto Request::url() const -> ref<str> { return m_url.as_ref(); }

auto Request::url_info() const -> const lihttpto::Url& { return m_url; }

auto Request::try_set_url(ref<str> input) -> rstd::Result<empty, lihttpto::UrlError> {
    auto parsed = lihttpto::Url::parse_http(input);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    m_url = rstd::move(parsed).unwrap();
    return Ok(empty {});
}

auto Request::header(ref<str> name) const -> Option<ref<lihttpto::HeaderValue>> {
    return m_header.get(name);
}

auto Request::header() const -> const lihttpto::Headers& { return m_header; }

auto Request::update_header(const lihttpto::Headers& h) -> Request& {
    for (const auto& field : h) (void)m_header.remove(field.name.as_str());
    for (const auto& field : h) m_header.push(field.clone());
    return *this;
}

auto Request::try_set_header(ref<str> name, ref<str> value)
    -> rstd::Result<empty, lihttpto::HeaderError> {
    return m_header.set(name, value);
}

Request& Request::remove_header(ref<str> name) {
    (void)m_header.remove(name);
    return *this;
}

auto Request::try_clone() const -> Result<Request> {
    auto body = m_body.try_clone();
    if (body.is_err()) return Err(rstd::move(body).unwrap_err());
    auto req      = Request {};
    req.m_method  = m_method.clone();
    req.m_body    = rstd::move(body).unwrap();
    req.m_url     = m_url.clone();
    req.m_header  = m_header.clone();
    req.m_options = m_options.clone();
    return Ok(rstd::move(req));
}

auto Request::redirected(lihttpto::Url target, bool use_get) const -> Result<Request> {
    auto out      = Request(rstd::move(target));
    out.m_header  = m_header.clone();
    out.m_options = m_options.clone();
    if (use_get) {
        ref<str> names[] = { "content-length"_str,    "content-type"_str, "content-encoding"_str,
                             "transfer-encoding"_str, "trailer"_str,      "expect"_str };
        for (auto name : names) out.remove_header(name);
    } else {
        if (m_body.reader().is_some() || m_body.stream().is_some())
            return Err(Error::Protocol(ProtocolError::RedirectBodyNotReplayable, nullptr));
        out.m_method = m_method.clone();
        out.m_body   = m_body.try_clone().unwrap();
    }
    return Ok(rstd::move(out));
}
