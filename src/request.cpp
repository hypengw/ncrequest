module;

#include <iostream>
#include <format>
#include <curl/curl.h>

module ncrequest;
import :request;

import ncrequest.curl;

using namespace ncrequest;

auto ncrequest::global_init(std::pmr::memory_resource* resource) -> std::error_code {
    return ncrequest::curl_init(resource);
}

Request::Request() noexcept
    : m_opts { req_opt::Timeout { .low_speed = 30, .connect_timeout = 180, .transfer_timeout = 0 },
               req_opt::Proxy {},
               req_opt::Tcp { .keepalive = false, .keepidle = 120, .keepintvl = 60 },
               req_opt::SSL { .verify_certificate = true },
               req_opt::Read {},
               req_opt::Share {} } {}
Request::Request(std::string_view url) noexcept: Request() { set_url(url); }
Request::~Request() noexcept {}
Request::Request(Request&&) noexcept            = default;
Request& Request::operator=(Request&&) noexcept = default;

std::string_view Request::url() const { return m_uri.uri; }

const URI& Request::url_info() const { return m_uri; }

Request& Request::set_url(std::string_view uri) {
    m_uri = URI::from(uri);
    return *this;
}

std::string Request::header(std::string_view name) const {
    if (m_header.contains(name)) {
        return m_header.at(std::string(name));
    }
    return std::string();
}

const Header& Request::header() const { return m_header; }

auto Request::update_header(const Header& h) -> Request& {
    for (auto& el : h) {
        m_header.insert_or_assign(el.first, el.second);
    }
    return *this;
}

Request& Request::set_header(std::string_view name, std::string_view value) {
    m_header.insert_or_assign(std::string(name), value);
    return *this;
}

Request& Request::remove_header(std::string_view name) {
    m_header.erase(std::string(name));
    return *this;
}

void Request::set_opt(const Header& header) { m_header = header; }

const_voidp Request::get_opt(usize idx) const {
    return RequestOpts::runtime_select(idx, [this]<usize I, typename T>() -> const_voidp {
        return &std::get<I>(m_opts);
    });
}
voidp Request::get_opt(usize idx) {
    return RequestOpts::runtime_select(idx, [this]<usize I, typename T>() -> voidp {
        return &std::get<I>(m_opts);
    });
}

void Request::set_opt(RequestOpt&& opt) {
    std::get<req_opt::Proxy>(m_opts);
    std::visit(helper::overloaded { [this](auto&& t) {
                   std::get<std::decay_t<decltype(t)>>(m_opts) = std::move(t);
               } },
               std::move(opt));
}

auto rstd::Impl<rstd::clone::Clone, ncrequest::Request>::clone(TraitPtr ptr) -> ncrequest::Request {
    auto  req    = ncrequest::Request {};
    auto& self   = ptr.as_ref<ncrequest::Request>();
    req.m_uri    = self.m_uri;
    req.m_header = self.m_header;
    req.m_opts   = std::apply(
        [](const auto&... elements) -> decltype(req.m_opts) {
            return { rstd::Impl<rstd::clone::Clone, std::decay_t<decltype(elements)>>::clone(
                &elements)... };
        },
        self.m_opts);
    constexpr auto ss = [](decltype(req.m_opts)& opts) {
        for (usize i = 0; i < std::tuple_size_v<std::decay_t<decltype(opts)>>; i++) {
            auto x = std::get<0>(opts);
        }
    };
    return req;
}

auto rstd::Impl<rstd::clone::Clone, ncrequest::req_opt::Share>::clone(TraitPtr ptr)
    -> ncrequest::req_opt::Share {
    auto& self = ptr.as_ref<ncrequest::req_opt::Share>();
    return { .share = self.share.clone() };
}