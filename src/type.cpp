module;

#include <format>
#include <string>
#include <vector>

module ncrequest.type;

using namespace ncrequest;

namespace
{
constexpr auto hex_digits = "0123456789ABCDEF";

constexpr bool is_unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

constexpr int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::string ncrequest::url_encode(std::string_view c) {
    std::string out;
    out.reserve(c.size() * 3);
    for (unsigned char value : c) {
        if (is_unreserved(value)) {
            out.push_back(static_cast<char>(value));
            continue;
        }
        out.push_back('%');
        out.push_back(hex_digits[value >> 4]);
        out.push_back(hex_digits[value & 0x0f]);
    }
    return out;
}
std::string ncrequest::url_decode(std::string_view c) {
    std::string out;
    out.reserve(c.size());
    for (usize i = 0; i < c.size(); ++i) {
        if (c[i] == '%' && i + 2 < c.size()) {
            auto high = hex_value(static_cast<unsigned char>(c[i + 1]));
            auto low  = hex_value(static_cast<unsigned char>(c[i + 2]));
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(c[i]);
    }
    return out;
}

bool CaseInsensitiveCompare::operator()(std::string_view a, std::string_view b) const noexcept {
    return helper::case_insensitive_compare(a, b) < 0;
}

std::string_view UrlParams::param(std::string_view name) const {
    if (auto it = m_params.find(name); it != m_params.end()) {
        return it->second.front();
    }
    return {};
}

UrlParams& UrlParams::set_param(std::string_view name, std::string_view val) {
    m_params.insert_or_assign(std::string(name), std::vector { std::string(val) });
    return *this;
}

auto UrlParams::params(std::string_view name) const -> std::vector<std::string_view> {
    if (auto it = m_params.find(name); it != m_params.end()) {
        return { it->second.begin(), it->second.end() };
    }
    return {};
}
auto UrlParams::is_array(std::string_view name) const -> bool {
    if (auto it = m_params.find(name); it != m_params.end()) {
        return it->second.size() > 1;
    }
    return false;
}
auto UrlParams::add_param(std::string_view name, std::string_view val) -> UrlParams& {
    if (auto it = m_params.find(name); it != m_params.end()) {
        it->second.push_back(std::string(val));
    } else {
        set_param(name, val);
    }
    return *this;
}

void UrlParams::decode(std::string_view) {}

std::string UrlParams::encode() const {
    std::string res;
    bool        first = true;
    for (auto& [k, v] : m_params) {
        if (! first)
            res.push_back('&');
        else
            first = false;

        if (v.size() > 1) {
            usize i = 0;
            for (auto& el : v) {
                res.append(std::format("{}[{}]={}", url_encode(k), i, url_encode(el)));
            }
        } else {
            res.append(std::format("{}={}", url_encode(k), url_encode(v.front())));
        }
    }
    return res;
}

/*
Url Url::from(std::string_view url_) {
    auto curlu_ =
        std::unique_ptr<CURLU, decltype(&::curl_url_cleanup)>(curl_url(), ::curl_url_cleanup);
    auto h = curlu_.get();

    Url o;

    auto flag = CURLU_URLDECODE;
    o.url     = url_;

    auto get_part = [&h, flag](CURLUPart p, std::string& o) {
        char* part;
        auto  rc = curl_url_get(h, p, &part, flag);
        if (rc == CURLUcode::CURLUE_OK) {
            o = part;
        }
        curl_free(part);
    };
    auto rc = curl_url_set(h, CURLUPART_URL, o.url.c_str(), 0);

    if (rc == CURLUcode::CURLUE_OK) {
        get_part(CURLUPART_HOST, o.host);
        get_part(CURLUPART_HOST, o.host);
        get_part(CURLUPART_SCHEME, o.scheme);
        get_part(CURLUPART_USER, o.user);
        get_part(CURLUPART_PASSWORD, o.password);
        get_part(CURLUPART_PORT, o.port);
        get_part(CURLUPART_PATH, o.path);
        get_part(CURLUPART_QUERY, o.query);
        get_part(CURLUPART_FRAGMENT, o.fragment);
    }

    return o;
}
*/
