export module ncrequest.http.parser.uri;
export import ncrequest.http.parser.cursor;
export import ncrequest.http.parser.ascii;

namespace ncrequest::http::parser::uri
{

using namespace rstd::prelude;

export struct OptionalSpan {
    Span span;
    bool present = false;
};

export struct UriReference {
    OptionalSpan scheme;
    OptionalSpan authority;
    OptionalSpan userinfo;
    OptionalSpan host;
    OptionalSpan port;
    Span         path;
    OptionalSpan query;
    OptionalSpan fragment;
};

namespace detail
{

auto failure(Expectation expected, Cursor& cursor, bool committed = true,
             bool incomplete = false) -> ParseResult<Span> {
    return Err(ParseFailure { rstd::move(expected), cursor.offset(), committed, incomplete });
}

auto is_scheme_char(u8 value) noexcept -> bool {
    return ascii::alpha(value) || ascii::digit(value) || value == '+' || value == '-' ||
           value == '.';
}

auto is_pchar_direct(u8 value) noexcept -> bool {
    return ascii::unreserved(value) || ascii::sub_delim(value) || value == ':' || value == '@';
}

auto is_path_noscheme_direct(u8 value) noexcept -> bool {
    return ascii::unreserved(value) || ascii::sub_delim(value) || value == '@';
}

auto is_userinfo_direct(u8 value) noexcept -> bool {
    return ascii::unreserved(value) || ascii::sub_delim(value) || value == ':';
}

auto is_reg_name_direct(u8 value) noexcept -> bool {
    return ascii::unreserved(value) || ascii::sub_delim(value);
}

auto is_query_direct(u8 value) noexcept -> bool {
    return is_pchar_direct(value) || value == '/' || value == '?';
}

auto is_ipvfuture_tail(u8 value) noexcept -> bool {
    return ascii::unreserved(value) || ascii::sub_delim(value) || value == ':';
}

auto consume_pct_encoded(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto mark  = take_byte(cursor, '%');
    if (mark.is_err()) return Err(rstd::move(mark).unwrap_err());

    for (usize i = 0; i < 2; ++i) {
        auto value = cursor.peek();
        if (value.is_none()) {
            return Err(ParseFailure { Expectation::PercentEncoding(), begin.offset, true, true });
        }
        if (! ascii::hexdig(*value)) {
            return Err(ParseFailure { Expectation::PercentEncoding(), begin.offset, true });
        }
        cursor.advance(1);
    }
    return Ok(cursor.span_from(begin));
}

auto consume_component_char(Cursor& cursor, Predicate direct, Expectation expected)
    -> ParseResult<Span> {
    auto next = cursor.peek();
    if (next.is_none()) {
        return Err(ParseFailure { rstd::move(expected), cursor.offset(), false, true });
    }
    if (*next == '%') return consume_pct_encoded(cursor);
    if (! direct(*next)) {
        return Err(ParseFailure { rstd::move(expected), cursor.offset() });
    }
    return take_if(cursor, direct);
}

auto parse_scheme(Cursor& cursor) -> ParseResult<OptionalSpan> {
    auto begin = cursor.mark();
    auto first = cursor.peek();
    if (first.is_none() || ! ascii::alpha(*first)) return Ok(OptionalSpan {});

    cursor.advance(1);
    (void)take_while(cursor, is_scheme_char);
    auto delimiter = cursor.peek();
    if (delimiter.is_none() || *delimiter != ':') {
        cursor.restore(begin);
        return Ok(OptionalSpan {});
    }

    auto span = cursor.span_from(begin);
    cursor.advance(1);
    return Ok(OptionalSpan { .span = span, .present = true });
}

auto parse_ipvfuture(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto v     = cursor.peek();
    if (v.is_none() || (*v != 'v' && *v != 'V')) {
        return failure(Expectation::IpLiteral(), cursor);
    }
    cursor.advance(1);

    auto version = take_while1(cursor, ascii::hexdig);
    if (version.is_err()) return failure(Expectation::IpLiteral(), cursor);
    auto dot = take_byte(cursor, '.');
    if (dot.is_err()) return failure(Expectation::IpLiteral(), cursor);
    auto tail = take_while1(cursor, is_ipvfuture_tail);
    if (tail.is_err()) return failure(Expectation::IpLiteral(), cursor);
    return Ok(cursor.span_from(begin));
}

auto parse_dec_octet(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto first = cursor.peek();
    if (first.is_none() || ! ascii::digit(*first)) {
        return failure(Expectation::Ipv4Address(), cursor, true, first.is_none());
    }

    usize value = 0;
    usize count = 0;
    while (auto next = cursor.peek()) {
        if (! ascii::digit(*next)) break;
        if (count == 3) return failure(Expectation::Ipv4Address(), cursor);
        value = value * 10 + static_cast<usize>(*next - '0');
        ++count;
        cursor.advance(1);
    }
    if ((count > 1 && cursor.input()[begin.offset] == '0') || value > 255) {
        cursor.restore(begin);
        return failure(Expectation::Ipv4Address(), cursor);
    }
    return Ok(cursor.span_from(begin));
}

auto parse_ipv4(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    for (usize part = 0; part < 4; ++part) {
        auto octet = parse_dec_octet(cursor);
        if (octet.is_err()) return Err(rstd::move(octet).unwrap_err());
        if (part == 3) break;
        auto dot = take_byte(cursor, '.');
        if (dot.is_err()) return failure(Expectation::Ipv4Address(), cursor);
    }
    return Ok(cursor.span_from(begin));
}

auto has_ipv4_tail(const Cursor& cursor) noexcept -> bool {
    auto input = cursor.input();
    for (usize i = cursor.offset(); i < input.len(); ++i) {
        auto value = input[i];
        if (value == '.') return true;
        if (value == ':' || value == ']') return false;
    }
    return false;
}

auto parse_h16(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    usize count = 0;
    while (auto next = cursor.peek()) {
        if (! ascii::hexdig(*next)) break;
        if (count == 4) return failure(Expectation::Ipv6Address(), cursor);
        ++count;
        cursor.advance(1);
    }
    if (count == 0) {
        return failure(Expectation::Ipv6Address(), cursor, true, cursor.at_end());
    }
    return Ok(cursor.span_from(begin));
}

auto parse_ipv6(Cursor& cursor) -> ParseResult<Span> {
    auto begin      = cursor.mark();
    usize groups    = 0;
    bool compressed = false;

    auto first = cursor.peek();
    if (first.is_some() && *first == ':') {
        cursor.advance(1);
        auto second = cursor.peek();
        if (second.is_none() || *second != ':') {
            return failure(Expectation::Ipv6Address(), cursor, true, second.is_none());
        }
        cursor.advance(1);
        compressed = true;
    }

    while (true) {
        auto next = cursor.peek();
        if (next.is_none()) {
            return failure(Expectation::Ipv6Address(), cursor, true, true);
        }
        if (*next == ']') break;
        if (groups >= 8) return failure(Expectation::Ipv6Address(), cursor);

        if (has_ipv4_tail(cursor)) {
            if (groups > 6) return failure(Expectation::Ipv6Address(), cursor);
            auto ipv4 = parse_ipv4(cursor);
            if (ipv4.is_err()) return Err(rstd::move(ipv4).unwrap_err());
            groups += 2;
            auto end = cursor.peek();
            if (end.is_none() || *end != ']') {
                return failure(Expectation::Ipv6Address(), cursor, true, end.is_none());
            }
            break;
        }

        auto group = parse_h16(cursor);
        if (group.is_err()) return Err(rstd::move(group).unwrap_err());
        ++groups;

        next = cursor.peek();
        if (next.is_none()) {
            return failure(Expectation::Ipv6Address(), cursor, true, true);
        }
        if (*next == ']') break;
        if (*next != ':') return failure(Expectation::Ipv6Address(), cursor);
        cursor.advance(1);

        next = cursor.peek();
        if (next.is_none()) {
            return failure(Expectation::Ipv6Address(), cursor, true, true);
        }
        if (*next == ':') {
            if (compressed) return failure(Expectation::Ipv6Address(), cursor);
            compressed = true;
            cursor.advance(1);
        }
    }

    if ((! compressed && groups != 8) || (compressed && groups >= 8)) {
        return failure(Expectation::Ipv6Address(), cursor);
    }
    return Ok(cursor.span_from(begin));
}

auto parse_ip_literal(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto address = delimited(
        cursor,
        [](Cursor& input) { return take_byte(input, '['); },
        [](Cursor& input) -> ParseResult<Span> {
            auto next = input.peek();
            if (next.is_none()) {
                return failure(Expectation::IpLiteral(), input, true, true);
            }
            return (*next == 'v' || *next == 'V') ? parse_ipvfuture(input)
                                                   : parse_ipv6(input);
        },
        [](Cursor& input) { return committed(take_byte(input, ']')); });
    if (address.is_err()) return Err(rstd::move(address).unwrap_err());
    return Ok(cursor.span_from(begin));
}

auto parse_userinfo(Cursor& cursor) -> ParseResult<OptionalSpan> {
    auto begin = cursor.mark();
    while (auto next = cursor.peek()) {
        if (*next == '@') {
            auto span = cursor.span_from(begin);
            cursor.advance(1);
            return Ok(OptionalSpan { .span = span, .present = true });
        }
        if (*next == '/' || *next == '?' || *next == '#') break;
        if (*next == '%') {
            auto encoded = consume_pct_encoded(cursor);
            if (encoded.is_err()) return Err(rstd::move(encoded).unwrap_err());
            continue;
        }
        if (! is_userinfo_direct(*next)) break;
        cursor.advance(1);
    }
    cursor.restore(begin);
    return Ok(OptionalSpan {});
}

auto parse_reg_name(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    while (auto next = cursor.peek()) {
        if (*next == ':' || *next == '/' || *next == '?' || *next == '#') break;
        auto value = consume_component_char(cursor, is_reg_name_direct, Expectation::Host());
        if (value.is_err()) {
            auto error = rstd::move(value).unwrap_err();
            error.commit();
            return Err(rstd::move(error));
        }
    }
    return Ok(cursor.span_from(begin));
}

auto parse_authority(Cursor& cursor, UriReference& output) -> ParseResult<Span> {
    auto begin = cursor.mark();

    auto userinfo = parse_userinfo(cursor);
    if (userinfo.is_err()) return Err(rstd::move(userinfo).unwrap_err());
    output.userinfo = rstd::move(userinfo).unwrap();

    ParseResult<Span> host = cursor.peek().is_some() && *cursor.peek() == '['
                                 ? parse_ip_literal(cursor)
                                 : parse_reg_name(cursor);
    if (host.is_err()) return Err(rstd::move(host).unwrap_err());
    output.host = OptionalSpan { .span = rstd::move(host).unwrap(), .present = true };

    auto next = cursor.peek();
    if (next.is_some() && *next == ':') {
        cursor.advance(1);
        auto port_begin = cursor.mark();
        (void)take_while(cursor, ascii::digit);
        output.port = OptionalSpan { .span = cursor.span_from(port_begin), .present = true };
    }

    next = cursor.peek();
    if (next.is_some() && *next != '/' && *next != '?' && *next != '#') {
        return failure(Expectation::Port(), cursor);
    }
    output.authority = OptionalSpan { .span = cursor.span_from(begin), .present = true };
    return Ok(output.authority.span);
}

auto parse_segment(Cursor& cursor, Predicate direct) -> ParseResult<Span> {
    auto begin = cursor.mark();
    while (auto next = cursor.peek()) {
        if (*next == '/' || *next == '?' || *next == '#') break;
        auto value = consume_component_char(cursor, direct, Expectation::Path());
        if (value.is_err()) {
            auto error = rstd::move(value).unwrap_err();
            error.commit();
            return Err(rstd::move(error));
        }
    }
    return Ok(cursor.span_from(begin));
}

auto parse_path_abempty(Cursor& cursor) -> ParseResult<Span> {
    return repeat(cursor, [](Cursor& input) -> ParseResult<Span> {
        auto begin = input.mark();
        auto slash = take_byte(input, '/');
        if (slash.is_err()) return Err(rstd::move(slash).unwrap_err());
        auto segment = parse_segment(input, is_pchar_direct);
        if (segment.is_err()) return Err(rstd::move(segment).unwrap_err());
        return Ok(input.span_from(begin));
    });
}

auto parse_path_absolute(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto slash = take_byte(cursor, '/');
    if (slash.is_err()) return Err(rstd::move(slash).unwrap_err());

    auto next = cursor.peek();
    if (next.is_some() && *next == '/') return failure(Expectation::Path(), cursor);
    if (next.is_some() && *next != '?' && *next != '#') {
        auto first = parse_segment(cursor, is_pchar_direct);
        if (first.is_err()) return Err(rstd::move(first).unwrap_err());
        while (auto separator = cursor.peek()) {
            if (*separator != '/') break;
            cursor.advance(1);
            auto segment = parse_segment(cursor, is_pchar_direct);
            if (segment.is_err()) return Err(rstd::move(segment).unwrap_err());
        }
    }
    return Ok(cursor.span_from(begin));
}

auto parse_path_rootless(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto first = parse_segment(cursor, is_pchar_direct);
    if (first.is_err()) return Err(rstd::move(first).unwrap_err());
    if (first.unwrap().is_empty()) return failure(Expectation::Path(), cursor);

    while (auto separator = cursor.peek()) {
        if (*separator != '/') break;
        cursor.advance(1);
        auto segment = parse_segment(cursor, is_pchar_direct);
        if (segment.is_err()) return Err(rstd::move(segment).unwrap_err());
    }
    return Ok(cursor.span_from(begin));
}

auto parse_path_noscheme(Cursor& cursor) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto first = parse_segment(cursor, is_path_noscheme_direct);
    if (first.is_err()) return Err(rstd::move(first).unwrap_err());
    if (first.unwrap().is_empty()) return failure(Expectation::Path(), cursor, false);

    while (auto separator = cursor.peek()) {
        if (*separator != '/') break;
        cursor.advance(1);
        auto segment = parse_segment(cursor, is_pchar_direct);
        if (segment.is_err()) return Err(rstd::move(segment).unwrap_err());
    }
    return Ok(cursor.span_from(begin));
}

auto parse_hier_part(Cursor& cursor, UriReference& output, bool relative)
    -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto next  = cursor.peek();
    if (next.is_some() && *next == '/' && cursor.remaining() >= 2 &&
        cursor.input()[cursor.offset() + 1] == '/') {
        cursor.advance(2);
        auto authority = parse_authority(cursor, output);
        if (authority.is_err()) return Err(rstd::move(authority).unwrap_err());
        auto path = parse_path_abempty(cursor);
        if (path.is_err()) return Err(rstd::move(path).unwrap_err());
        output.path = rstd::move(path).unwrap();
        return Ok(cursor.span_from(begin));
    }

    if (next.is_some() && *next == '/') {
        auto path = parse_path_absolute(cursor);
        if (path.is_err()) return Err(rstd::move(path).unwrap_err());
        output.path = rstd::move(path).unwrap();
        return Ok(cursor.span_from(begin));
    }

    if (next.is_none() || *next == '?' || *next == '#') {
        output.path = Span { cursor.offset(), cursor.offset() };
        return Ok(cursor.span_from(begin));
    }

    auto path = relative ? parse_path_noscheme(cursor) : parse_path_rootless(cursor);
    if (path.is_err()) return Err(rstd::move(path).unwrap_err());
    output.path = rstd::move(path).unwrap();
    return Ok(cursor.span_from(begin));
}

auto parse_suffix(Cursor& cursor, UriReference& output) -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto query = optional(cursor, [&output](Cursor& input) -> ParseResult<Span> {
        auto delimiter = take_byte(input, '?');
        if (delimiter.is_err()) return Err(rstd::move(delimiter).unwrap_err());
        auto query_begin = input.mark();
        while (auto value = input.peek()) {
            if (*value == '#') break;
            auto parsed = consume_component_char(input, is_query_direct, Expectation::Query());
            if (parsed.is_err()) {
                auto error = rstd::move(parsed).unwrap_err();
                error.commit();
                return Err(rstd::move(error));
            }
        }
        auto span = input.span_from(query_begin);
        output.query = OptionalSpan { .span = span, .present = true };
        return Ok(span);
    });
    if (query.is_err()) return Err(rstd::move(query).unwrap_err());

    auto fragment = optional(cursor, [&output](Cursor& input) -> ParseResult<Span> {
        auto delimiter = take_byte(input, '#');
        if (delimiter.is_err()) return Err(rstd::move(delimiter).unwrap_err());
        auto fragment_begin = input.mark();
        while (! input.at_end()) {
            auto parsed =
                consume_component_char(input, is_query_direct, Expectation::Fragment());
            if (parsed.is_err()) {
                auto error = rstd::move(parsed).unwrap_err();
                error.commit();
                return Err(rstd::move(error));
            }
        }
        auto span = input.span_from(fragment_begin);
        output.fragment = OptionalSpan { .span = span, .present = true };
        return Ok(span);
    });
    if (fragment.is_err()) return Err(rstd::move(fragment).unwrap_err());
    return Ok(cursor.span_from(begin));
}

} // namespace detail

export auto parse(ref<str> input) -> ParseResult<UriReference> {
    auto cursor = Cursor { input };
    auto output = UriReference {};

    auto scheme = detail::parse_scheme(cursor);
    if (scheme.is_err()) return Err(rstd::move(scheme).unwrap_err());
    output.scheme = rstd::move(scheme).unwrap();

    auto hierarchy = detail::parse_hier_part(cursor, output, ! output.scheme.present);
    if (hierarchy.is_err()) return Err(rstd::move(hierarchy).unwrap_err());

    auto suffix = detail::parse_suffix(cursor, output);
    if (suffix.is_err()) return Err(rstd::move(suffix).unwrap_err());

    auto end = take_end(cursor);
    if (end.is_err()) return Err(rstd::move(end).unwrap_err());
    return Ok(rstd::move(output));
}

export auto validate_ipv4(ref<str> input) -> ParseResult<Span> {
    auto cursor = Cursor { input };
    auto parsed = detail::parse_ipv4(cursor);
    if (parsed.is_err()) return parsed;
    auto end = take_end(cursor);
    if (end.is_err()) {
        return Err(ParseFailure { Expectation::Ipv4Address(), cursor.offset(), true });
    }
    return parsed;
}

} // namespace ncrequest::http::parser::uri
