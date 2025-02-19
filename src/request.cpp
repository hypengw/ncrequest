#include "request.hpp"
#include "request_p.hpp"

#include <iostream>
#include <fmt/core.h>

#include "session.hpp"
#include "session_p.hpp"

#include "response.hpp"
#include "curl_multi.hpp"
#include "curl_easy.hpp"

#include "ncrequest/type_list.hpp"

using namespace ncrequest;

std::error_code ncrequest::global_init() {
    return ::make_error_code(curl_global_init(CURL_GLOBAL_ALL));
}

Request::Request() noexcept: m_d(std::make_unique<Private>(this)) {}
Request::Request(std::string_view url) noexcept: Request() { set_url(url); }
Request::~Request() noexcept {}
Request::Request(const Request& o): m_d(std::make_unique<Private>(*(o.m_d))) { m_d->m_q = this; }
Request& Request::operator=(const Request& o) {
    *(m_d)   = *(o.m_d);
    m_d->m_q = this;
    return *this;
}

Request::Private::Private(Request* q)
    : m_q(q),
      m_opts { req_opt::Timeout { .low_speed = 30, .connect_timeout = 180, .transfer_timeout = 0 },
               req_opt::Proxy {},
               req_opt::Tcp { .keepalive = false, .keepidle = 120, .keepintvl = 60 },
               req_opt::SSL { .verify_certificate = true },
               {},
               {} } {}
Request::Private::~Private() {}

std::string_view Request::url() const {
    C_D(const Request);
    return d->m_uri.uri;
}

const URI& Request::url_info() const {
    C_D(const Request);
    return d->m_uri;
}

Request& Request::set_url(std::string_view uri) {
    C_D(Request);
    d->m_uri = URI::from(uri);
    return *this;
}

std::string Request::header(std::string_view name) const {
    C_D(const Request);
    if (d->m_header.contains(name)) {
        return d->m_header.at(std::string(name));
    }
    return std::string();
}

const Header& Request::header() const {
    C_D(const Request);
    return d->m_header;
}

auto Request::update_header(const Header& h) -> Request& {
    C_D(Request);
    for (auto& el : h) {
        d->m_header.insert_or_assign(el.first, el.second);
    }
    return *this;
}

Request& Request::set_header(std::string_view name, std::string_view value) {
    C_D(Request);
    d->m_header.insert_or_assign(std::string(name), value);
    return *this;
}

Request& Request::remove_header(std::string_view name) {
    C_D(Request);
    d->m_header.erase(std::string(name));
    return *this;
}

void Request::set_opt(const Header& header) {
    C_D(Request);
    d->m_header = header;
}

const_voidp Request::get_opt(usize idx) const {
    C_D(const Request);
    return RequestOpts::runtime_select(idx, [d]<usize I, typename T>() -> const_voidp {
        return &std::get<I>(d->m_opts);
    });
}
voidp Request::get_opt(usize idx) {
    C_D(Request);
    return RequestOpts::runtime_select(idx, [d]<usize I, typename T>() -> voidp {
        return &std::get<I>(d->m_opts);
    });
}

void Request::set_opt(const RequestOpt& opt) {
    C_D(Request);

    std::get<req_opt::Proxy>(d->m_opts);
    std::visit(helper::overloaded { [d](const auto& t) {
                   std::get<std::decay_t<decltype(t)>>(d->m_opts) = t;
               } },
               opt);
}