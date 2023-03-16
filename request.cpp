#include "request.h"
#include "request_p.h"
#include "session.h"
#include "session_p.h"

#include "response.h"
#include "curl_multi.h"
#include "curl_easy.h"

#include <iostream>

#include <fmt/core.h>

using namespace request;

std::error_code request::global_init() {
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
    : m_q(q), m_low_speed(30), m_connect_timeout(180), m_transfer_timeout(0) {}
Request::Private::~Private() {}

std::string_view Request::url() const {
    C_D(const Request);
    return d->m_url.url;
}

const Url& Request::url_info() const {
    C_D(const Request);
    return d->m_url;
}

Request& Request::set_url(std::string_view url) {
    C_D(Request);
    d->m_url = Url::from(url);
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

Request& Request::set_header(std::string_view name, std::string_view value) {
    C_D(Request);
    d->m_header.insert({ std::string(name), std::string(value) });
    return *this;
}

void Request::set_option(const Header& header) {
    C_D(Request);
    d->m_header = header;
}

i32 Request::connect_timeout() const {
    C_D(const Request);
    return d->m_connect_timeout;
}
Request& Request::set_connect_timeout(i32 val) {
    C_D(Request);
    d->m_connect_timeout = val;
    return *this;
}
i32 Request::transfer_timeout() const {
    C_D(const Request);
    return d->m_transfer_timeout;
}
Request& Request::set_transfer_timeout(i32 val) {
    C_D(Request);
    d->m_transfer_timeout = val;
    return *this;
}

i32 Request::transfer_low_speed() const {
    C_D(const Request);
    return d->m_transfer_timeout;
}
Request& Request::set_transfer_low_speed(i32 val) {
    C_D(Request);
    d->m_transfer_timeout = val;
    return *this;
}
