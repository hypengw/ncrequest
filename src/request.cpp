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

auto ncrequest::global_init(std::pmr::memory_resource* resource) -> Result<rstd::empty> {
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
    : m_opts { req_opt::Timeout {
                   .low_speed = i64(30), .connect_timeout = i64(180), .transfer_timeout = i64() },
               req_opt::Proxy {},
               req_opt::Tcp { .keepalive = false, .keepidle = i64(120), .keepintvl = i64(60) },
               req_opt::SSL { .verify_certificate = true },
               req_opt::Read {},
               req_opt::Share {} } {}
Request::Request(http::Url url) noexcept: Request() { m_url = rstd::move(url); }
Request::~Request() noexcept {}
Request::Request(Request&&) noexcept            = default;
Request& Request::operator=(Request&&) noexcept = default;

auto Request::from_url(rstd::ref<rstd::str> input) -> rstd::Result<Request, http::UrlError> {
    auto parsed = http::Url::parse_http(input);
    if (parsed.is_err()) return rstd::Err(rstd::move(parsed).unwrap_err());
    return rstd::Ok(Request { rstd::move(parsed).unwrap() });
}

std::string_view Request::url() const { return rstd::cppstd::as_string_view(m_url.as_ref()); }

auto Request::url_info() const -> const http::Url& { return m_url; }

auto Request::try_set_url(rstd::ref<rstd::str> input) -> rstd::Result<rstd::empty, http::UrlError> {
    auto parsed = http::Url::parse_http(input);
    if (parsed.is_err()) return rstd::Err(rstd::move(parsed).unwrap_err());
    m_url = rstd::move(parsed).unwrap();
    return rstd::Ok(rstd::empty {});
}

std::string Request::header(std::string_view name) const {
    auto name_text = rstd::cppstd::as_str(name);
    if (name_text.is_err()) return {};
    auto value = m_header.get(rstd::move(name_text).unwrap());
    if (value.is_none()) return {};
    auto bytes = (**value).as_bytes();
    return { reinterpret_cast<const char*>(bytes.as_raw_ptr()), bytes.len().to_primitive() };
}

auto Request::header() const -> const http::Header& { return m_header; }

auto Request::update_header(const http::Header& h) -> Request& {
    auto removals = h.iter();
    for (auto field = removals.next(); field.is_some(); field = removals.next()) {
        (void)m_header.remove((**field).name().as_ref());
    }

    auto additions = h.iter();
    for (auto field = additions.next(); field.is_some(); field = additions.next()) {
        m_header.append((**field).clone());
    }
    return *this;
}

auto Request::try_set_header(rstd::ref<rstd::str> name, rstd::ref<rstd::str> value)
    -> rstd::Result<rstd::empty, http::HeaderError> {
    return m_header.set(name, value);
}

Request& Request::remove_header(rstd::ref<rstd::str> name) {
    (void)m_header.remove(name);
    return *this;
}

void Request::set_opt(const http::Header& header) { m_header = header.clone(); }

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
    req.m_url    = self.m_url.clone();
    req.m_header = self.m_header.clone();
    req.m_opts   = as<rstd::clone::Clone>(self.m_opts).clone();
    return req;
}

auto req_opt::Share::clone() const -> ncrequest::req_opt::Share {
    return { .share = share.clone() };
}
