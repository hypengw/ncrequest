module;
#include <rstd/enum.hpp>
module ncrequest;
import :request;
import cppstd;
import rstd.cppstd;

#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
import ncrequest.curl;
#endif

using namespace ncrequest;
using namespace rstd::literals;
using rstd::bytes::Bytes;
using rstd::clone::Clone;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using std::pmr::memory_resource;

auto ncrequest::global_init(memory_resource* resource) -> Result<rstd::empty> {
#if defined(NCREQUEST_CLIENT_BACKEND_CURL)
    auto initialized = ncrequest::curl_init(resource);
    if (initialized.is_err()) {
        return Err(rstd::into<Error>(rstd::move(initialized).unwrap_err()));
    }
    return Ok(rstd::empty {});
#else
    (void)resource;
    return Ok(rstd::empty {});
#endif
}

Request::Request() noexcept
    : m_method(lihttpto::Method::parse("GET"_str).unwrap()),
      m_opts { req_opt::Timeout {
                   .low_speed = i64(30), .connect_timeout = i64(180), .transfer_timeout = i64() },
               req_opt::Proxy {},
               req_opt::Tcp { .keepalive = false, .keepidle = i64(120), .keepintvl = i64(60) },
               req_opt::SSL { .verify_certificate = true },
               req_opt::Read {},
               req_opt::Share {} } {}
Request::Request(lihttpto::Url url) noexcept: Request() { m_url = rstd::move(url); }
Request::~Request() noexcept {}
Request::Request(Request&&) noexcept            = default;
Request& Request::operator=(Request&&) noexcept = default;

auto Request::from_url(rstd::ref<rstd::str> input) -> rstd::Result<Request, lihttpto::UrlError> {
    auto parsed = lihttpto::Url::parse_http(input);
    if (parsed.is_err()) return rstd::Err(rstd::move(parsed).unwrap_err());
    return rstd::Ok(Request { rstd::move(parsed).unwrap() });
}

auto Request::method() const -> const lihttpto::Method& { return m_method; }
auto Request::set_method(lihttpto::Method method) -> Request& {
    m_method = rstd::move(method);
    return *this;
}
auto Request::try_set_method(rstd::ref<rstd::str> method)
    -> rstd::Result<rstd::empty, lihttpto::HttpParseError> {
    auto parsed = lihttpto::Method::parse(method);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    m_method = rstd::move(parsed).unwrap();
    return Ok(rstd::empty {});
}
auto Request::body() const -> const rstd::Option<Bytes>& { return m_body; }
auto Request::set_body(Bytes body) -> Request& {
    m_body = Some(rstd::move(body));
    return *this;
}
auto Request::clear_body() -> Request& {
    m_body = None();
    return *this;
}
auto Request::validate() const -> Result<rstd::empty> {
    if (m_url.as_ref().size() == usize()) return Err(Error::InvalidState("request URL is empty"));
    const auto& reader = get_opt<req_opt::Read>();
    if (reader.callback && m_body.is_some())
        return Err(Error::InvalidState("request cannot combine byte body and read callback"));
    if (reader.callback && m_method.as_ref() != "POST"_str)
        return Err(Error::Unsupported("read callback requires POST"));
    if (m_method.as_ref() == "HEAD"_str && m_body.is_some() && m_body->size() != usize())
        return Err(Error::InvalidState("HEAD request cannot contain a body"));
    return Ok(rstd::empty {});
}

std::string_view Request::url() const { return as_string_view(m_url.as_ref()); }

auto Request::url_info() const -> const lihttpto::Url& { return m_url; }

auto Request::try_set_url(rstd::ref<rstd::str> input)
    -> rstd::Result<rstd::empty, lihttpto::UrlError> {
    auto parsed = lihttpto::Url::parse_http(input);
    if (parsed.is_err()) return rstd::Err(rstd::move(parsed).unwrap_err());
    m_url = rstd::move(parsed).unwrap();
    return rstd::Ok(rstd::empty {});
}

std::string Request::header(std::string_view name) const {
    auto name_text = as_str(name);
    if (name_text.is_err()) return {};
    auto value = m_header.get(rstd::move(name_text).unwrap());
    if (value.is_none()) return {};
    auto bytes = (**value).as_slice();
    return { reinterpret_cast<const char*>(bytes.as_raw_ptr()), bytes.len().to_primitive() };
}

auto Request::header() const -> const lihttpto::Headers& { return m_header; }

auto Request::update_header(const lihttpto::Headers& h) -> Request& {
    for (const auto& field : h) (void)m_header.remove(field.name.as_str());
    for (const auto& field : h) m_header.push(field.clone());
    return *this;
}

auto Request::try_set_header(rstd::ref<rstd::str> name, rstd::ref<rstd::str> value)
    -> rstd::Result<rstd::empty, lihttpto::HeaderError> {
    return m_header.set(name, value);
}

Request& Request::remove_header(rstd::ref<rstd::str> name) {
    (void)m_header.remove(name);
    return *this;
}

void Request::set_opt(const lihttpto::Headers& header) { m_header = header.clone(); }

void Request::set_opt(RequestOpt&& opt) {
    RSTD_MATCH(rstd::move(opt)) {
        RSTD_CASE(Timeout, value) { m_opts.get<req_opt::Timeout>() = rstd::move(value); }
        RSTD_CASE(Proxy, value) { m_opts.get<req_opt::Proxy>() = rstd::move(value); }
        RSTD_CASE(Tcp, value) { m_opts.get<req_opt::Tcp>() = rstd::move(value); }
        RSTD_CASE(SSL, value) { m_opts.get<req_opt::SSL>() = rstd::move(value); }
        RSTD_CASE(Read, value) { m_opts.get<req_opt::Read>() = rstd::move(value); }
        RSTD_CASE(Share, value) { m_opts.get<req_opt::Share>() = rstd::move(value); }
    }
}

auto Request::clone() const -> ncrequest::Request {
    auto  req    = ncrequest::Request {};
    auto& self   = *this;
    req.m_method = self.m_method.clone();
    if (self.m_body.is_some()) req.m_body = Some(Bytes::copy_from_slice(self.m_body->as_slice()));
    req.m_url    = self.m_url.clone();
    req.m_header = self.m_header.clone();
    req.m_opts   = as<Clone>(self.m_opts).clone();
    return req;
}

auto req_opt::Share::clone() const -> ncrequest::req_opt::Share {
    return { .share = share.clone() };
}
