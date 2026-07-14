module;
#include <rstd/enum.hpp>

export module ncrequest.http.parser.http1;
export import ncrequest.http.parser.cursor;
export import ncrequest.http.parser.ascii;
import ncrequest.http.parser.uri;

export namespace ncrequest::http::parser::http1
{

using namespace rstd::prelude;

struct RequestLine {
    Span method;
    Span target;
    u8   major;
    u8   minor;
};

struct StatusLine {
    u8   major;
    u8   minor;
    u16  status;
    Span reason;
};

#define NCREQUEST_HTTP1_START_LINE_VARIANTS(V) \
    V(Request, (RequestLine value;))            \
    V(Response, (StatusLine value;))

struct StartLine {
    RSTD_ENUM_BODY(StartLine, NCREQUEST_HTTP1_START_LINE_VARIANTS)
};

#undef NCREQUEST_HTTP1_START_LINE_VARIANTS

struct FieldLine {
    Span name;
    Span value;
};

namespace detail
{

struct Version {
    u8 major;
    u8 minor;
};

auto failure(Expectation expected, Cursor& cursor, bool committed = true,
             bool incomplete = false) -> ParseResult<Span> {
    return Err(ParseFailure { rstd::move(expected), cursor.offset(), committed, incomplete });
}

auto is_ows(u8 value) noexcept -> bool { return value == ' ' || value == '\t'; }

auto is_reason_byte(u8 value) noexcept -> bool {
    return value == '\t' || (value >= 0x20 && value <= 0x7e) || value >= 0x80;
}

auto is_target_byte(u8 value) noexcept -> bool {
    return value >= 0x21 && value <= 0x7e && value != '#';
}

auto parse_crlf(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto parsed = sequence(
        cursor,
        [](Cursor& input) { return take_byte(input, '\r'); },
        [](Cursor& input) { return take_byte(input, '\n'); });
    if (parsed.is_ok()) return parsed;
    auto error = rstd::move(parsed).unwrap_err();
    return Err(ParseFailure { Expectation::CrLf(), begin.offset, true,
                              error.is_incomplete() });
}

auto parse_version(Cursor& cursor) -> ParseResult<Version> {
    auto literal = take_literal(cursor, "HTTP/");
    if (literal.is_err()) {
        auto error = rstd::move(literal).unwrap_err();
        return Err(ParseFailure { Expectation::HttpVersion(), error.offset(), true,
                                  error.is_incomplete() });
    }

    auto major = cursor.peek();
    if (major.is_none() || ! ascii::digit(*major)) {
        return Err(ParseFailure { Expectation::HttpVersion(), cursor.offset(), true,
                                  major.is_none() });
    }
    cursor.advance(1);

    auto dot = take_byte(cursor, '.');
    if (dot.is_err()) {
        return Err(ParseFailure { Expectation::HttpVersion(), cursor.offset(), true,
                                  cursor.at_end() });
    }

    auto minor = cursor.peek();
    if (minor.is_none() || ! ascii::digit(*minor)) {
        return Err(ParseFailure { Expectation::HttpVersion(), cursor.offset(), true,
                                  minor.is_none() });
    }
    cursor.advance(1);
    return Ok(Version { static_cast<u8>(*major - '0'), static_cast<u8>(*minor - '0') });
}

auto equals(slice<u8> input, Span span, const char* expected) noexcept -> bool {
    auto size = rstd::strlen(expected);
    if (span.size() != size) return false;
    for (usize offset = 0; offset < size; ++offset) {
        if (input[span.begin + offset] != static_cast<u8>(expected[offset])) return false;
    }
    return true;
}

auto valid_connect_target(slice<u8> input, Span target) noexcept -> bool {
    usize colon = target.end;
    if (input[target.begin] == '[') {
        usize close = target.begin + 1;
        while (close < target.end && input[close] != ']') ++close;
        if (close == target.end || close + 1 >= target.end || input[close + 1] != ':') {
            return false;
        }
        colon = close + 1;
    } else {
        for (usize offset = target.begin; offset < target.end; ++offset) {
            if (input[offset] == ':') colon = offset;
        }
        if (colon == target.begin || colon == target.end) return false;
    }
    if (colon + 1 == target.end) return false;
    for (usize offset = colon + 1; offset < target.end; ++offset) {
        if (! ascii::digit(input[offset])) return false;
    }
    return true;
}

auto validate_request_target(slice<u8> input, Span method, Span target)
    -> ParseResult<empty> {
    if (target.size() == 1 && input[target.begin] == '*') return Ok(empty {});
    if (equals(input, method, "CONNECT") && valid_connect_target(input, target)) {
        return Ok(empty {});
    }

    auto value = ref<str>::from_raw_parts(input.as_raw_ptr() + target.begin, target.size());
    auto parsed = uri::parse(value);
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err();
        return Err(ParseFailure { Expectation::RequestTarget(), target.begin + error.offset(),
                                  true, error.is_incomplete() });
    }

    auto uri = rstd::move(parsed).unwrap();
    if (uri.fragment.present) {
        return Err(ParseFailure { Expectation::RequestTarget(), target.begin + uri.fragment.span.begin,
                                  true });
    }
    if (input[target.begin] == '/') {
        if (! uri.scheme.present && ! uri.authority.present) return Ok(empty {});
    } else if (uri.scheme.present) {
        return Ok(empty {});
    }
    return Err(ParseFailure { Expectation::RequestTarget(), target.begin, true });
}

auto parse_request_line(slice<u8> input) -> ParseResult<StartLine> {
    auto cursor = Cursor { input };
    auto method = take_while1(cursor, ascii::tchar);
    if (method.is_err()) {
        return Err(ParseFailure { Expectation::Method(), cursor.offset(), false,
                                  cursor.at_end() });
    }

    auto method_span = rstd::move(method).unwrap();
    auto method_space = take_byte(cursor, ' ');
    if (method_space.is_err()) {
        return Err(ParseFailure { Expectation::Method(), cursor.offset(), true,
                                  cursor.at_end() });
    }

    auto target = take_while1(cursor, is_target_byte);
    if (target.is_err()) {
        return Err(ParseFailure { Expectation::RequestTarget(), cursor.offset(), true,
                                  cursor.at_end() });
    }
    auto target_span = rstd::move(target).unwrap();
    auto target_space = take_byte(cursor, ' ');
    if (target_space.is_err()) {
        return Err(ParseFailure { Expectation::RequestTarget(), cursor.offset(), true,
                                  cursor.at_end() });
    }

    auto valid_target = validate_request_target(input, method_span, target_span);
    if (valid_target.is_err()) return Err(rstd::move(valid_target).unwrap_err());

    auto version = parse_version(cursor);
    if (version.is_err()) return Err(rstd::move(version).unwrap_err());
    auto crlf = parse_crlf(cursor);
    if (crlf.is_err()) return Err(rstd::move(crlf).unwrap_err());
    auto end = take_end(cursor);
    if (end.is_err()) return Err(rstd::move(end).unwrap_err());

    auto parsed_version = rstd::move(version).unwrap();
    return Ok(StartLine::Request(RequestLine {
        .method = method_span,
        .target = target_span,
        .major  = parsed_version.major,
        .minor  = parsed_version.minor,
    }));
}

auto parse_status_line(slice<u8> input) -> ParseResult<StartLine> {
    auto cursor  = Cursor { input };
    auto version = parse_version(cursor);
    if (version.is_err()) return Err(rstd::move(version).unwrap_err());

    auto version_space = take_byte(cursor, ' ');
    if (version_space.is_err()) {
        return Err(ParseFailure { Expectation::HttpVersion(), cursor.offset(), true,
                                  cursor.at_end() });
    }

    auto status_begin = cursor.offset();
    u16  status       = 0;
    for (usize digit = 0; digit < 3; ++digit) {
        auto value = cursor.peek();
        if (value.is_none() || ! ascii::digit(*value)) {
            return Err(ParseFailure { Expectation::StatusCode(), cursor.offset(), true,
                                      value.is_none() });
        }
        status = static_cast<u16>(status * 10 + (*value - '0'));
        cursor.advance(1);
    }
    if (status < 100) {
        return Err(ParseFailure { Expectation::StatusCode(), status_begin, true });
    }

    auto status_space = take_byte(cursor, ' ');
    if (status_space.is_err()) {
        return Err(ParseFailure { Expectation::StatusCode(), cursor.offset(), true,
                                  cursor.at_end() });
    }

    auto reason_begin = cursor.mark();
    while (auto value = cursor.peek()) {
        if (*value == '\r') break;
        if (! is_reason_byte(*value)) {
            return Err(ParseFailure { Expectation::ReasonPhrase(), cursor.offset(), true });
        }
        cursor.advance(1);
    }
    auto reason = cursor.span_from(reason_begin);
    auto crlf   = parse_crlf(cursor);
    if (crlf.is_err()) return Err(rstd::move(crlf).unwrap_err());
    auto end = take_end(cursor);
    if (end.is_err()) return Err(rstd::move(end).unwrap_err());

    auto parsed_version = rstd::move(version).unwrap();
    return Ok(StartLine::Response(StatusLine {
        .major  = parsed_version.major,
        .minor  = parsed_version.minor,
        .status = status,
        .reason = reason,
    }));
}

} // namespace detail

[[nodiscard]]
auto parse_start_line(slice<u8> input) -> ParseResult<StartLine> {
    constexpr u8 prefix[] = { 'H', 'T', 'T', 'P', '/' };
    bool status = input.len() >= 5;
    for (usize offset = 0; status && offset < 5; ++offset) {
        status = input[offset] == prefix[offset];
    }
    return status ? detail::parse_status_line(input) : detail::parse_request_line(input);
}

[[nodiscard]]
auto parse_field_line(slice<u8> input) -> ParseResult<FieldLine> {
    auto cursor = Cursor { input };
    auto name   = take_while1(cursor, ascii::tchar);
    if (name.is_err()) {
        return Err(ParseFailure { Expectation::HeaderName(), cursor.offset(), false,
                                  cursor.at_end() });
    }
    auto name_span = rstd::move(name).unwrap();

    auto colon = take_byte(cursor, ':');
    if (colon.is_err()) {
        return Err(ParseFailure { Expectation::HeaderName(), cursor.offset(), true,
                                  cursor.at_end() });
    }
    (void)take_while(cursor, detail::is_ows);

    auto value_begin = cursor.mark();
    while (auto value = cursor.peek()) {
        if (*value == '\r') break;
        if (! detail::is_reason_byte(*value)) {
            return Err(ParseFailure { Expectation::HeaderValue(), cursor.offset(), true });
        }
        cursor.advance(1);
    }
    auto value = cursor.span_from(value_begin);
    while (value.end > value.begin && detail::is_ows(input[value.end - 1])) --value.end;

    auto crlf = detail::parse_crlf(cursor);
    if (crlf.is_err()) return Err(rstd::move(crlf).unwrap_err());
    auto end = take_end(cursor);
    if (end.is_err()) return Err(rstd::move(end).unwrap_err());
    return Ok(FieldLine { .name = name_span, .value = value });
}

} // namespace ncrequest::http::parser::http1
