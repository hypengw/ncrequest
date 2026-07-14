module;
#include <rstd/enum.hpp>

export module ncrequest.http.parser.cursor;
export import rstd;

export namespace ncrequest::http::parser
{

using namespace rstd::prelude;

#define NCREQUEST_PARSE_EXPECTATION_VARIANTS(V) \
    V(Byte)                                      \
    V(Literal)                                   \
    V(AsciiClass)                                \
    V(PercentEncoding)                           \
    V(Scheme)                                    \
    V(Host)                                      \
    V(IpLiteral)                                 \
    V(Ipv4Address)                               \
    V(Ipv6Address)                               \
    V(Port)                                      \
    V(Path)                                      \
    V(Query)                                     \
    V(Fragment)                                  \
    V(Method)                                    \
    V(RequestTarget)                             \
    V(HttpVersion)                               \
    V(StatusCode)                                \
    V(ReasonPhrase)                              \
    V(HeaderName)                                \
    V(HeaderValue)                               \
    V(CrLf)                                      \
    V(End)

struct Expectation {
    RSTD_TAG_ENUM_BODY(Expectation, NCREQUEST_PARSE_EXPECTATION_VARIANTS)
};

#undef NCREQUEST_PARSE_EXPECTATION_VARIANTS

struct Span {
    usize begin = 0;
    usize end   = 0;

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return end - begin;
    }

    [[nodiscard]]
    constexpr auto is_empty() const noexcept -> bool {
        return begin == end;
    }
};

struct Mark {
    usize offset = 0;
};

class ParseFailure {
public:
    constexpr ParseFailure(Expectation expected, usize offset, bool committed = false,
                           bool incomplete = false) noexcept
        : expected_(rstd::move(expected)),
          offset_(offset),
          committed_(committed),
          incomplete_(incomplete) {}

    [[nodiscard]]
    constexpr auto expected() const noexcept -> const Expectation& {
        return expected_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

    [[nodiscard]]
    constexpr auto is_committed() const noexcept -> bool {
        return committed_;
    }

    [[nodiscard]]
    constexpr auto is_incomplete() const noexcept -> bool {
        return incomplete_;
    }

    constexpr auto commit() noexcept -> ParseFailure& {
        committed_ = true;
        return *this;
    }

private:
    Expectation expected_;
    usize       offset_;
    bool        committed_;
    bool        incomplete_;
};

template<typename T>
using ParseResult = Result<T, ParseFailure>;

class Cursor {
public:
    explicit constexpr Cursor(ref<str> input) noexcept
        : input_(rstd::slice<u8>::from_raw_parts(input.data(), input.size())) {}

    explicit constexpr Cursor(rstd::slice<u8> input) noexcept: input_(input) {}

    [[nodiscard]]
    constexpr auto input() const noexcept -> rstd::slice<u8> {
        return input_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

    [[nodiscard]]
    constexpr auto remaining() const noexcept -> usize {
        return input_.len() - offset_;
    }

    [[nodiscard]]
    constexpr auto at_end() const noexcept -> bool {
        return offset_ == input_.len();
    }

    [[nodiscard]]
    constexpr auto peek() const noexcept -> Option<u8> {
        if (at_end()) return None();
        auto value = input_[offset_];
        return Some(rstd::move(value));
    }

    [[nodiscard]]
    constexpr auto mark() const noexcept -> Mark {
        return Mark { offset_ };
    }

    constexpr void restore(Mark mark) noexcept { offset_ = mark.offset; }

    constexpr auto advance(usize count) noexcept -> bool {
        if (count > remaining()) return false;
        offset_ += count;
        return true;
    }

    [[nodiscard]]
    constexpr auto span_from(Mark mark) const noexcept -> Span {
        return Span { mark.offset, offset_ };
    }

    [[nodiscard]]
    constexpr auto slice(Span span) const noexcept -> rstd::slice<u8> {
        return rstd::slice<u8>::from_raw_parts(input_.as_raw_ptr() + span.begin, span.size());
    }

private:
    rstd::slice<u8> input_;
    usize    offset_ = 0;
};

using Predicate = bool (*)(u8) noexcept;

[[nodiscard]]
constexpr auto farther(ParseFailure left, ParseFailure right) noexcept -> ParseFailure {
    if (right.offset() > left.offset()) return right;
    if (right.offset() < left.offset()) return left;
    if (right.is_committed() && ! left.is_committed()) return right;
    if (left.is_committed() && ! right.is_committed()) return left;
    if (! right.is_incomplete() && left.is_incomplete()) return right;
    return left;
}

[[nodiscard]]
constexpr auto take_byte(Cursor& cursor, u8 expected) noexcept -> ParseResult<Span> {
    auto mark = cursor.mark();
    auto next = cursor.peek();
    if (next.is_none()) {
        return Err(ParseFailure { Expectation::Byte(), cursor.offset(), false, true });
    }
    if (*next != expected) {
        return Err(ParseFailure { Expectation::Byte(), cursor.offset() });
    }
    cursor.advance(1);
    return Ok(cursor.span_from(mark));
}

[[nodiscard]]
constexpr auto take_if(Cursor& cursor, Predicate predicate) noexcept -> ParseResult<Span> {
    auto mark = cursor.mark();
    auto next = cursor.peek();
    if (next.is_none()) {
        return Err(ParseFailure { Expectation::AsciiClass(), cursor.offset(), false, true });
    }
    if (! predicate(*next)) {
        return Err(ParseFailure { Expectation::AsciiClass(), cursor.offset() });
    }
    cursor.advance(1);
    return Ok(cursor.span_from(mark));
}

[[nodiscard]]
constexpr auto take_while(Cursor& cursor, Predicate predicate) noexcept -> Span {
    auto mark = cursor.mark();
    while (auto next = cursor.peek()) {
        if (! predicate(*next)) break;
        cursor.advance(1);
    }
    return cursor.span_from(mark);
}

[[nodiscard]]
constexpr auto take_while1(Cursor& cursor, Predicate predicate) noexcept -> ParseResult<Span> {
    auto span = take_while(cursor, predicate);
    if (! span.is_empty()) return Ok(span);
    return Err(ParseFailure { Expectation::AsciiClass(), cursor.offset(), false,
                              cursor.at_end() });
}

[[nodiscard]]
inline auto take_literal(Cursor& cursor, ref<str> literal) noexcept -> ParseResult<Span> {
    auto mark = cursor.mark();
    for (usize i = 0; i < literal.size(); ++i) {
        auto next = cursor.peek();
        if (next.is_none()) {
            cursor.restore(mark);
            return Err(ParseFailure { Expectation::Literal(), mark.offset + i, false, true });
        }
        if (*next != literal.data()[i]) {
            cursor.restore(mark);
            return Err(ParseFailure { Expectation::Literal(), mark.offset + i });
        }
        cursor.advance(1);
    }
    return Ok(cursor.span_from(mark));
}

[[nodiscard]]
constexpr auto take_end(Cursor& cursor) noexcept -> ParseResult<Span> {
    if (cursor.at_end()) {
        auto mark = cursor.mark();
        return Ok(cursor.span_from(mark));
    }
    return Err(ParseFailure { Expectation::End(), cursor.offset() });
}

template<typename T>
[[nodiscard]]
constexpr auto committed(ParseResult<T> result) -> ParseResult<T> {
    if (result.is_ok()) return result;
    auto error = rstd::move(result).unwrap_err();
    error.commit();
    return Err(rstd::move(error));
}

template<typename Rule>
using RuleValue = typename rstd::mtp::invoke_result_t<Rule, Cursor&>::value_type;

template<typename Rule>
[[nodiscard]]
constexpr auto attempt(Cursor& cursor, Rule rule) -> ParseResult<RuleValue<Rule>> {
    auto mark   = cursor.mark();
    auto result = rule(cursor);
    if (result.is_ok()) return result;

    auto error = rstd::move(result).unwrap_err();
    if (! error.is_committed()) cursor.restore(mark);
    return Err(rstd::move(error));
}

template<typename Rule>
[[nodiscard]]
constexpr auto optional(Cursor& cursor, Rule rule)
    -> ParseResult<Option<RuleValue<Rule>>> {
    auto mark   = cursor.mark();
    auto result = rule(cursor);
    if (result.is_ok()) {
        return Ok(Some(rstd::move(result).unwrap()));
    }

    auto error = rstd::move(result).unwrap_err();
    if (error.is_committed()) return Err(rstd::move(error));
    cursor.restore(mark);
    if (error.offset() != mark.offset) return Err(rstd::move(error));
    return Ok(None<RuleValue<Rule>>());
}

template<typename First, typename Second>
[[nodiscard]]
constexpr auto sequence(Cursor& cursor, First first, Second second)
    -> ParseResult<Span> {
    auto begin = cursor.mark();
    auto left  = first(cursor);
    if (left.is_err()) {
        auto error = rstd::move(left).unwrap_err();
        if (! error.is_committed()) cursor.restore(begin);
        return Err(rstd::move(error));
    }

    auto right = second(cursor);
    if (right.is_err()) {
        auto error = rstd::move(right).unwrap_err();
        if (! error.is_committed()) cursor.restore(begin);
        return Err(rstd::move(error));
    }
    return Ok(cursor.span_from(begin));
}

template<typename Rule>
[[nodiscard]]
constexpr auto repeat(Cursor& cursor, Rule rule, usize minimum = 0,
                      usize maximum = static_cast<usize>(-1)) -> ParseResult<Span> {
    auto begin = cursor.mark();
    usize count = 0;
    while (count < maximum) {
        auto item_begin = cursor.mark();
        auto item       = rule(cursor);
        if (item.is_ok()) {
            if (cursor.offset() == item_begin.offset) {
                return Err(ParseFailure { Expectation::End(), cursor.offset(), true });
            }
            ++count;
            continue;
        }

        auto error = rstd::move(item).unwrap_err();
        if (error.is_committed() || error.offset() != item_begin.offset) {
            return Err(rstd::move(error));
        }
        cursor.restore(item_begin);
        if (count < minimum) return Err(rstd::move(error));
        break;
    }
    return Ok(cursor.span_from(begin));
}

template<typename Open, typename Inner, typename Close>
[[nodiscard]]
constexpr auto delimited(Cursor& cursor, Open open, Inner inner, Close close)
    -> ParseResult<RuleValue<Inner>> {
    auto begin = cursor.mark();
    auto opened = open(cursor);
    if (opened.is_err()) {
        auto error = rstd::move(opened).unwrap_err();
        if (! error.is_committed()) cursor.restore(begin);
        return Err(rstd::move(error));
    }

    auto value = inner(cursor);
    if (value.is_err()) {
        auto error = rstd::move(value).unwrap_err();
        if (! error.is_committed()) cursor.restore(begin);
        return Err(rstd::move(error));
    }

    auto closed = close(cursor);
    if (closed.is_err()) {
        auto error = rstd::move(closed).unwrap_err();
        if (! error.is_committed()) cursor.restore(begin);
        return Err(rstd::move(error));
    }
    return rstd::move(value);
}

template<typename First, typename Second>
[[nodiscard]]
constexpr auto choice(Cursor& cursor, First first, Second second)
    -> decltype(first(cursor)) {
    auto first_result = attempt(cursor, first);
    if (first_result.is_ok()) return first_result;

    auto first_error = rstd::move(first_result).unwrap_err();
    if (first_error.is_committed()) return Err(rstd::move(first_error));
    auto second_result = attempt(cursor, second);
    if (second_result.is_ok()) return second_result;
    auto second_error = rstd::move(second_result).unwrap_err();
    return Err(farther(rstd::move(first_error), rstd::move(second_error)));
}

} // namespace ncrequest::http::parser
