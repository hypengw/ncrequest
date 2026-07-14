module ncrequest;
import :http_cookie;
import ncrequest.http.parser;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

namespace
{

struct ParsedCookie {
    parser::Span name;
    parser::Span value;
    bool         quoted;
};

constexpr auto cookie_octet(u8 value) noexcept -> bool {
    return value == 0x21 || (value >= 0x23 && value <= 0x2b) ||
           (value >= 0x2d && value <= 0x3a) ||
           (value >= 0x3c && value <= 0x5b) ||
           (value >= 0x5d && value <= 0x7e);
}

constexpr auto ows(u8 value) noexcept -> bool { return value == ' ' || value == '\t'; }

constexpr auto ascii_lower(u8 value) noexcept -> u8 {
    if (value >= 'A' && value <= 'Z') return value + ('a' - 'A');
    return value;
}

auto same(ref<str> left, ref<str> right) noexcept -> bool {
    if (left.size() != right.size()) return false;
    for (usize offset = 0; offset < left.size(); ++offset) {
        if (left.data()[offset] != right.data()[offset]) return false;
    }
    return true;
}

auto same_ascii_case(ref<str> left, ref<str> right) noexcept -> bool {
    if (left.size() != right.size()) return false;
    for (usize offset = 0; offset < left.size(); ++offset) {
        if (ascii_lower(left.data()[offset]) != ascii_lower(right.data()[offset])) return false;
    }
    return true;
}

auto copy_ascii(slice<u8> input, parser::Span span) -> String {
    auto value = String::make();
    for (usize offset = span.begin; offset < span.end; ++offset) {
        value.push_back(input[offset]);
    }
    return value;
}

auto trim_ows(slice<u8> input, parser::Span span) noexcept -> parser::Span {
    while (span.begin < span.end && ows(input[span.begin])) ++span.begin;
    while (span.end > span.begin && ows(input[span.end - 1])) --span.end;
    return span;
}

auto parse_cookie_pair(parser::Cursor& cursor) -> rstd::Result<ParsedCookie, CookieError> {
    auto name = parser::take_while(cursor, parser::ascii::tchar);
    if (name.is_empty()) {
        auto kind = cursor.peek().is_some() && *cursor.peek() == '='
                        ? CookieErrorKind::EmptyName()
                        : CookieErrorKind::InvalidName();
        return Err(CookieError { rstd::move(kind), cursor.offset() });
    }
    if (cursor.at_end()) {
        return Err(CookieError { CookieErrorKind::InvalidSyntax(), cursor.offset() });
    }
    if (*cursor.peek() != '=') {
        return Err(CookieError { CookieErrorKind::InvalidName(), cursor.offset() });
    }
    cursor.advance(1);

    bool quoted = false;
    if (! cursor.at_end() && *cursor.peek() == '"') {
        quoted = true;
        cursor.advance(1);
    }

    auto value = parser::take_while(cursor, cookie_octet);
    if (quoted) {
        if (cursor.at_end() || *cursor.peek() != '"') {
            return Err(CookieError { CookieErrorKind::InvalidValue(), cursor.offset() });
        }
        cursor.advance(1);
    }

    if (! cursor.at_end() && *cursor.peek() != ';') {
        return Err(CookieError { CookieErrorKind::InvalidValue(), cursor.offset() });
    }
    return Ok(ParsedCookie { name, value, quoted });
}

auto valid_attribute_byte(u8 value) noexcept -> bool {
    return value >= 0x20 && value <= 0x7e && value != ';';
}

} // namespace

Cookie::Cookie(String name, String value, bool quoted) noexcept
    : name_(rstd::move(name)), value_(rstd::move(value)), quoted_(quoted) {}

auto Cookie::parse(ref<str> input) -> rstd::Result<Cookie, CookieError> {
    return parse_bytes(rstd::str_::as_bytes(input));
}

auto Cookie::parse_bytes(slice<u8> input) -> rstd::Result<Cookie, CookieError> {
    auto cursor = parser::Cursor { input };
    auto parsed = parse_cookie_pair(cursor);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    if (! cursor.at_end()) {
        return Err(CookieError { CookieErrorKind::InvalidSyntax(), cursor.offset() });
    }
    auto pair = rstd::move(parsed).unwrap();
    return Ok(Cookie { copy_ascii(input, pair.name), copy_ascii(input, pair.value),
                       pair.quoted });
}

auto Cookie::name() const noexcept -> ref<str> { return name_.as_str(); }

auto Cookie::value() const noexcept -> ref<str> { return value_.as_str(); }

auto Cookie::is_quoted() const noexcept -> bool { return quoted_; }

auto Cookie::encode() const -> String {
    auto output = String::make();
    output.push_str(name());
    output.push_back('=');
    if (quoted_) output.push_back('"');
    output.push_str(value());
    if (quoted_) output.push_back('"');
    return output;
}

auto Cookie::clone() const -> Cookie {
    return Cookie { name_.clone(), value_.clone(), quoted_ };
}

CookieIter::CookieIter(const Cookie* current, const Cookie* end) noexcept
    : current_(current), end_(end) {}

auto CookieIter::next() noexcept -> Option<Item> {
    if (current_ == end_) return None();
    auto* value = current_++;
    return Some(ref<Cookie>::from_raw_parts(value));
}

auto CookieHeader::parse(ref<str> input) -> rstd::Result<CookieHeader, CookieError> {
    return parse_bytes(rstd::str_::as_bytes(input));
}

auto CookieHeader::parse_bytes(slice<u8> input) -> rstd::Result<CookieHeader, CookieError> {
    auto result = CookieHeader {};
    auto outer  = trim_ows(input, parser::Span { 0, input.len() });
    if (outer.is_empty()) {
        return Err(CookieError { CookieErrorKind::EmptyName(), outer.begin });
    }
    auto payload = slice<u8>::from_raw_parts(input.as_raw_ptr() + outer.begin, outer.size());
    auto cursor  = parser::Cursor { payload };

    for (;;) {
        auto parsed = parse_cookie_pair(cursor);
        if (parsed.is_err()) {
            auto error = rstd::move(parsed).unwrap_err();
            return Err(CookieError { error.kind(), outer.begin + error.offset() });
        }
        auto pair = rstd::move(parsed).unwrap();
        result.cookies_.push(Cookie { copy_ascii(payload, pair.name),
                                      copy_ascii(payload, pair.value), pair.quoted });

        if (cursor.at_end()) break;
        cursor.advance(1);
        (void)parser::take_while(cursor, ows);
        if (cursor.at_end()) {
            return Err(CookieError { CookieErrorKind::EmptyName(),
                                     outer.begin + cursor.offset() });
        }
    }
    return Ok(rstd::move(result));
}

void CookieHeader::add(Cookie cookie) { cookies_.push(rstd::move(cookie)); }

auto CookieHeader::get(ref<str> name) const noexcept -> Option<ref<Cookie>> {
    for (auto const& cookie : cookies_) {
        if (same(cookie.name(), name)) {
            return Some(ref<Cookie>::from_raw_parts(&cookie));
        }
    }
    return None();
}

auto CookieHeader::len() const noexcept -> usize { return cookies_.len(); }

auto CookieHeader::is_empty() const noexcept -> bool { return cookies_.is_empty(); }

auto CookieHeader::iter() const noexcept -> CookieIter {
    return CookieIter { cookies_.begin(), cookies_.end() };
}

auto CookieHeader::encode() const -> String {
    auto output = String::make();
    for (usize offset = 0; offset < cookies_.len(); ++offset) {
        if (offset != 0) output.push_str("; ");
        auto encoded = cookies_[offset].encode();
        output.push_str(encoded.as_str());
    }
    return output;
}

auto CookieHeader::clone() const -> CookieHeader {
    auto result     = CookieHeader {};
    result.cookies_ = cookies_.clone();
    return result;
}

CookieAttribute::CookieAttribute(String name, Option<String> value) noexcept
    : name_(rstd::move(name)), value_(rstd::move(value)) {}

auto CookieAttribute::name() const noexcept -> ref<str> { return name_.as_str(); }

auto CookieAttribute::value() const noexcept -> Option<ref<str>> {
    if (value_.is_none()) return None();
    return Some(value_->as_str());
}

auto CookieAttribute::name_equals(ref<str> name) const noexcept -> bool {
    return same_ascii_case(name_.as_str(), name);
}

auto CookieAttribute::clone() const -> CookieAttribute {
    return CookieAttribute { name_.clone(), value_.clone() };
}

CookieAttributeIter::CookieAttributeIter(const CookieAttribute* current,
                                         const CookieAttribute* end) noexcept
    : current_(current), end_(end) {}

auto CookieAttributeIter::next() noexcept -> Option<Item> {
    if (current_ == end_) return None();
    auto* value = current_++;
    return Some(ref<CookieAttribute>::from_raw_parts(value));
}

SetCookie::SetCookie(Cookie cookie, Vec<CookieAttribute> attributes) noexcept
    : cookie_(rstd::move(cookie)), attributes_(rstd::move(attributes)) {}

auto SetCookie::parse(ref<str> input) -> rstd::Result<SetCookie, CookieError> {
    return parse_bytes(rstd::str_::as_bytes(input));
}

auto SetCookie::parse_bytes(slice<u8> input) -> rstd::Result<SetCookie, CookieError> {
    auto cursor = parser::Cursor { input };
    auto parsed = parse_cookie_pair(cursor);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    auto pair = rstd::move(parsed).unwrap();
    auto cookie = Cookie { copy_ascii(input, pair.name), copy_ascii(input, pair.value),
                           pair.quoted };
    auto attributes = Vec<CookieAttribute>::make();

    while (! cursor.at_end()) {
        cursor.advance(1);
        (void)parser::take_while(cursor, ows);
        auto attribute_begin = cursor.offset();
        while (! cursor.at_end() && *cursor.peek() != ';') cursor.advance(1);
        auto attribute = trim_ows(input, parser::Span { attribute_begin, cursor.offset() });
        if (attribute.is_empty()) {
            return Err(CookieError { CookieErrorKind::InvalidAttribute(), attribute.begin });
        }

        usize separator = attribute.begin;
        while (separator < attribute.end && input[separator] != '=') ++separator;
        auto name = trim_ows(input, parser::Span { attribute.begin, separator });
        if (name.is_empty()) {
            return Err(CookieError { CookieErrorKind::InvalidAttribute(), name.begin });
        }
        for (usize offset = name.begin; offset < name.end; ++offset) {
            if (! valid_attribute_byte(input[offset])) {
                return Err(CookieError { CookieErrorKind::InvalidAttribute(), offset });
            }
        }

        auto value = Option<String> { None() };
        if (separator < attribute.end) {
            auto value_span = trim_ows(input, parser::Span { separator + 1, attribute.end });
            for (usize offset = value_span.begin; offset < value_span.end; ++offset) {
                if (! valid_attribute_byte(input[offset])) {
                    return Err(CookieError { CookieErrorKind::InvalidAttribute(), offset });
                }
            }
            value = Some(copy_ascii(input, value_span));
        }
        attributes.push(CookieAttribute { copy_ascii(input, name), rstd::move(value) });
    }

    return Ok(SetCookie { rstd::move(cookie), rstd::move(attributes) });
}

auto SetCookie::cookie() const noexcept -> const Cookie& { return cookie_; }

auto SetCookie::attribute(ref<str> name) const noexcept
    -> Option<ref<CookieAttribute>> {
    for (auto const& attribute : attributes_) {
        if (attribute.name_equals(name)) {
            return Some(ref<CookieAttribute>::from_raw_parts(&attribute));
        }
    }
    return None();
}

auto SetCookie::attributes() const noexcept -> CookieAttributeIter {
    return CookieAttributeIter { attributes_.begin(), attributes_.end() };
}

auto SetCookie::secure() const noexcept -> bool { return attribute("secure").is_some(); }

auto SetCookie::http_only() const noexcept -> bool { return attribute("httponly").is_some(); }

auto SetCookie::encode() const -> String {
    auto output = cookie_.encode();
    for (auto const& attribute : attributes_) {
        output.push_str("; ");
        output.push_str(attribute.name());
        auto value = attribute.value();
        if (value.is_some()) {
            output.push_back('=');
            output.push_str(*value);
        }
    }
    return output;
}

auto SetCookie::clone() const -> SetCookie {
    return SetCookie { cookie_.clone(), attributes_.clone() };
}

} // namespace ncrequest::http
