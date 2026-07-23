module;
#include <rstd/enum.hpp>

export module ncrequest.http.parser.http1;
export import ncrequest.http.parser.cursor;
export import ncrequest.http.parser.ascii;
import ncrequest.http.parser.uri;

export namespace ncrequest::http::parser::http1
{

using namespace rstd::prelude;
using namespace rstd::literals;

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

struct StartLine {
    RSTD_ENUM(StartLine, (Request, (RequestLine value;)), (Response, (StatusLine value;)))
};

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

auto failure(Expectation expected, Cursor& cursor, bool committed = true, bool incomplete = false)
    -> ParseResult<Span> {
    return Err(ParseFailure { rstd::move(expected), cursor.offset(), committed, incomplete });
}

auto is_ows(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return raw == ' ' || raw == '\t';
}

auto is_reason_byte(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return raw == '\t' || (raw >= 0x20 && raw <= 0x7e) || raw >= 0x80;
}

auto is_target_byte(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return raw >= 0x21 && raw <= 0x7e && raw != '#';
}

auto parse_crlf(Cursor& cursor) -> ParseResult<Span> {
    auto begin  = cursor.mark();
    auto parsed = sequence(
        cursor,
        [](Cursor& input) {
            return take_byte(input, u8('\r'));
        },
        [](Cursor& input) {
            return take_byte(input, u8('\n'));
        });
    if (parsed.is_ok()) return parsed;
    auto error = rstd::move(parsed).unwrap_err();
    return Err(ParseFailure { Expectation::CrLf(), begin.offset, true, error.is_incomplete() });
}

auto parse_version(Cursor& cursor) -> ParseResult<Version> {
    auto literal = take_literal(cursor, "HTTP/"_str);
    if (literal.is_err()) {
        auto error = rstd::move(literal).unwrap_err();
        return Err(ParseFailure {
            Expectation::HttpVersion(), error.offset(), true, error.is_incomplete() });
    }

    auto major = cursor.peek();
    if (major.is_none() || ! ascii::digit(*major)) {
        return Err(
            ParseFailure { Expectation::HttpVersion(), cursor.offset(), true, major.is_none() });
    }
    cursor.advance(usize(1));

    auto dot = take_byte(cursor, u8('.'));
    if (dot.is_err()) {
        return Err(
            ParseFailure { Expectation::HttpVersion(), cursor.offset(), true, cursor.at_end() });
    }

    auto minor = cursor.peek();
    if (minor.is_none() || ! ascii::digit(*minor)) {
        return Err(
            ParseFailure { Expectation::HttpVersion(), cursor.offset(), true, minor.is_none() });
    }
    cursor.advance(usize(1));
    return Ok(Version { u8(major->to_primitive() - '0'), u8(minor->to_primitive() - '0') });
}

auto equals(slice<u8> input, Span span, const char* expected) noexcept -> bool {
    auto size = usize(rstd::strlen(expected));
    if (span.size() != size) return false;
    for (usize offset {}; offset < size; ++offset) {
        if (input[span.begin + offset] != u8(expected[offset.to_primitive()])) {
            return false;
        }
    }
    return true;
}

auto valid_connect_target(slice<u8> input, Span target) noexcept -> bool {
    usize colon = target.end;
    if (input[target.begin] == u8('[')) {
        usize close = target.begin + usize(1);
        while (close < target.end && input[close] != u8(']')) ++close;
        if (close == target.end || close + usize(1) >= target.end ||
            input[close + usize(1)] != u8(':')) {
            return false;
        }
        colon = close + usize(1);
    } else {
        for (usize offset = target.begin; offset < target.end; ++offset) {
            if (input[offset] == u8(':')) colon = offset;
        }
        if (colon == target.begin || colon == target.end) return false;
    }
    if (colon + usize(1) == target.end) return false;
    for (usize offset = colon + usize(1); offset < target.end; ++offset) {
        if (! ascii::digit(input[offset])) return false;
    }
    return true;
}

auto validate_request_target(slice<u8> input, Span method, Span target) -> ParseResult<empty> {
    auto target_bytes = slice<u8>::from_raw_parts(
        input.as_raw_ptr() + target.begin.to_primitive(), target.size());
    auto target_text = rstd::str_::from_utf8(target_bytes);
    if (target_text.is_err()) {
        return Err(ParseFailure { Expectation::RequestTarget(),
                                  target.begin + target_text.unwrap_err().valid_up_to(), true });
    }

    if (target.size() == usize(1) && input[target.begin] == u8('*')) return Ok(empty {});
    if (equals(input, method, "CONNECT") && valid_connect_target(input, target)) {
        return Ok(empty {});
    }

    auto parsed = uri::parse(rstd::move(target_text).unwrap());
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err();
        return Err(ParseFailure { Expectation::RequestTarget(),
                                  target.begin + error.offset(),
                                  true,
                                  error.is_incomplete() });
    }

    auto uri = rstd::move(parsed).unwrap();
    if (uri.fragment.present) {
        return Err(ParseFailure {
            Expectation::RequestTarget(), target.begin + uri.fragment.span.begin, true });
    }
    if (input[target.begin] == u8('/')) {
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
        return Err(ParseFailure { Expectation::Method(), cursor.offset(), false, cursor.at_end() });
    }

    auto method_span  = rstd::move(method).unwrap();
    auto method_space = take_byte(cursor, u8(' '));
    if (method_space.is_err()) {
        return Err(ParseFailure { Expectation::Method(), cursor.offset(), true, cursor.at_end() });
    }

    auto target = take_while1(cursor, is_target_byte);
    if (target.is_err()) {
        return Err(
            ParseFailure { Expectation::RequestTarget(), cursor.offset(), true, cursor.at_end() });
    }
    auto target_span  = rstd::move(target).unwrap();
    auto target_space = take_byte(cursor, u8(' '));
    if (target_space.is_err()) {
        return Err(
            ParseFailure { Expectation::RequestTarget(), cursor.offset(), true, cursor.at_end() });
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

    auto version_space = take_byte(cursor, u8(' '));
    if (version_space.is_err()) {
        return Err(
            ParseFailure { Expectation::HttpVersion(), cursor.offset(), true, cursor.at_end() });
    }

    auto           status_begin = cursor.offset();
    rstd::uint16_t status {};
    for (usize digit {}; digit < usize(3); ++digit) {
        auto value = cursor.peek();
        if (value.is_none() || ! ascii::digit(*value)) {
            return Err(
                ParseFailure { Expectation::StatusCode(), cursor.offset(), true, value.is_none() });
        }
        status = static_cast<rstd::uint16_t>(status * 10 + value->to_primitive() - '0');
        cursor.advance(usize(1));
    }
    if (status < 100) {
        return Err(ParseFailure { Expectation::StatusCode(), status_begin, true });
    }

    auto status_space = take_byte(cursor, u8(' '));
    if (status_space.is_err()) {
        return Err(
            ParseFailure { Expectation::StatusCode(), cursor.offset(), true, cursor.at_end() });
    }

    auto reason_begin = cursor.mark();
    while (auto value = cursor.peek()) {
        if (value->to_primitive() == '\r') break;
        if (! is_reason_byte(*value)) {
            return Err(ParseFailure { Expectation::ReasonPhrase(), cursor.offset(), true });
        }
        cursor.advance(usize(1));
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
        .status = u16(status),
        .reason = reason,
    }));
}

} // namespace detail

[[nodiscard]]
auto parse_start_line(slice<u8> input) -> ParseResult<StartLine> {
    constexpr auto prefix =
        rstd::array<u8, 5> { u8('H'), u8('T'), u8('T'), u8('P'), u8('/') };
    bool           status   = input.len() >= usize(5);
    for (usize offset {}; status && offset < usize(5); ++offset) {
        status = input[offset] == prefix[offset];
    }
    return status ? detail::parse_status_line(input) : detail::parse_request_line(input);
}

[[nodiscard]]
auto parse_field_line(slice<u8> input) -> ParseResult<FieldLine> {
    auto cursor = Cursor { input };
    auto name   = take_while1(cursor, ascii::tchar);
    if (name.is_err()) {
        return Err(
            ParseFailure { Expectation::HeaderName(), cursor.offset(), false, cursor.at_end() });
    }
    auto name_span = rstd::move(name).unwrap();

    auto colon = take_byte(cursor, u8(':'));
    if (colon.is_err()) {
        return Err(
            ParseFailure { Expectation::HeaderName(), cursor.offset(), true, cursor.at_end() });
    }
    (void)take_while(cursor, detail::is_ows);

    auto value_begin = cursor.mark();
    while (auto value = cursor.peek()) {
        if (value->to_primitive() == '\r') break;
        if (! detail::is_reason_byte(*value)) {
            return Err(ParseFailure { Expectation::HeaderValue(), cursor.offset(), true });
        }
        cursor.advance(usize(1));
    }
    auto value = cursor.span_from(value_begin);
    while (value.end > value.begin &&
           detail::is_ows(input[value.end - usize(1)])) {
        --value.end;
    }

    auto crlf = detail::parse_crlf(cursor);
    if (crlf.is_err()) return Err(rstd::move(crlf).unwrap_err());
    auto end = take_end(cursor);
    if (end.is_err()) return Err(rstd::move(end).unwrap_err());
    return Ok(FieldLine { .name = name_span, .value = value });
}

} // namespace ncrequest::http::parser::http1
