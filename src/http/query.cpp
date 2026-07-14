module ncrequest;
import :http_query;
import ncrequest.http.parser;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

namespace
{

constexpr char hex_digits[] = "0123456789ABCDEF";

auto hex_value(u8 value) noexcept -> Option<u8> {
    if (value >= '0' && value <= '9') return Some(static_cast<u8>(value - '0'));
    if (value >= 'a' && value <= 'f') return Some(static_cast<u8>(value - 'a' + 10));
    if (value >= 'A' && value <= 'F') return Some(static_cast<u8>(value - 'A' + 10));
    return None();
}

auto decode(ref<str> input, bool plus_as_space) -> rstd::Result<String, QueryError> {
    auto bytes          = Vec<u8>::with_capacity(input.size());
    auto source_offsets = Vec<usize>::with_capacity(input.size());
    auto cursor         = parser::Cursor { input };

    while (! cursor.at_end()) {
        auto source_offset = cursor.offset();
        auto value         = *cursor.peek();
        if (value == '%') {
            cursor.advance(1);
            auto high_offset = cursor.offset();
            auto high = cursor.peek();
            if (high.is_none()) {
                return Err(QueryError { QueryErrorKind::InvalidPercentEncoding(),
                                        source_offset });
            }
            cursor.advance(1);
            auto low = cursor.peek();
            if (low.is_none()) {
                return Err(QueryError { QueryErrorKind::InvalidPercentEncoding(),
                                        source_offset });
            }
            cursor.advance(1);

            auto high_value = hex_value(*high);
            auto low_value  = hex_value(*low);
            if (high_value.is_none() || low_value.is_none()) {
                auto error_offset = high_value.is_none() ? high_offset : high_offset + 1;
                return Err(QueryError { QueryErrorKind::InvalidPercentEncoding(), error_offset });
            }
            value = static_cast<u8>((*high_value << 4) | *low_value);
        } else {
            cursor.advance(1);
            if (plus_as_space && value == '+') value = ' ';
        }
        bytes.push(rstd::move(value));
        source_offsets.push(rstd::move(source_offset));
    }

    auto decoded = String::from_utf8(rstd::move(bytes));
    if (decoded.is_err()) {
        auto decoded_offset = decoded.unwrap_err().valid_up_to();
        auto source_offset = decoded_offset < source_offsets.len()
                                 ? source_offsets[decoded_offset]
                                 : input.size();
        return Err(QueryError { QueryErrorKind::InvalidUtf8(), source_offset });
    }
    return Ok(rstd::move(decoded).unwrap());
}

auto encode(ref<str> input, bool space_as_plus) -> String {
    auto output = String::make();
    for (usize offset = 0; offset < input.size(); ++offset) {
        auto value = input.data()[offset];
        if (parser::ascii::unreserved(value)) {
            output.push_back(value);
        } else if (space_as_plus && value == ' ') {
            output.push_back('+');
        } else {
            output.push_back('%');
            output.push_back(hex_digits[value >> 4]);
            output.push_back(hex_digits[value & 0x0f]);
        }
    }
    return output;
}

auto same(ref<str> left, ref<str> right) noexcept -> bool {
    if (left.size() != right.size()) return false;
    for (usize offset = 0; offset < left.size(); ++offset) {
        if (left.data()[offset] != right.data()[offset]) return false;
    }
    return true;
}

auto rebase(QueryError error, usize base) -> QueryError {
    auto kind = error.kind().is_InvalidPercentEncoding()
                    ? QueryErrorKind::InvalidPercentEncoding()
                    : QueryErrorKind::InvalidUtf8();
    return QueryError { rstd::move(kind), base + error.offset() };
}

} // namespace

QueryPair::QueryPair(String name, String value) noexcept
    : name_(rstd::move(name)), value_(rstd::move(value)) {}

auto QueryPair::name() const noexcept -> ref<str> { return name_.as_str(); }

auto QueryPair::value() const noexcept -> ref<str> { return value_.as_str(); }

auto QueryPair::clone() const -> QueryPair { return QueryPair { name_.clone(), value_.clone() }; }

QueryIter::QueryIter(const QueryPair* current, const QueryPair* end) noexcept
    : current_(current), end_(end) {}

auto QueryIter::next() noexcept -> Option<Item> {
    if (current_ == end_) return None();
    auto* value = current_++;
    return Some(ref<QueryPair>::from_raw_parts(value));
}

QueryValues::QueryValues(const QueryPair* current,
                         const QueryPair* end,
                         const QueryPair* match) noexcept
    : current_(current), end_(end), match_(match) {}

auto QueryValues::next() noexcept -> Option<Item> {
    if (match_ == nullptr) return None();
    while (current_ != end_) {
        auto* pair = current_++;
        if (same(pair->name(), match_->name())) return Some(pair->value());
    }
    return None();
}

auto QueryParams::parse_query(ref<str> input) -> rstd::Result<QueryParams, QueryError> {
    return parse_with_mode(input, false);
}

auto QueryParams::parse_form(ref<str> input) -> rstd::Result<QueryParams, QueryError> {
    return parse_with_mode(input, true);
}

auto QueryParams::parse_with_mode(ref<str> input, bool form)
    -> rstd::Result<QueryParams, QueryError> {
    auto result = QueryParams {};
    usize pair_begin = 0;
    while (pair_begin <= input.size()) {
        usize pair_end = pair_begin;
        while (pair_end < input.size() && input.data()[pair_end] != '&') ++pair_end;
        if (pair_end == pair_begin && pair_end == input.size()) break;

        usize separator = pair_begin;
        while (separator < pair_end && input.data()[separator] != '=') ++separator;
        auto name_text = ref<str>::from_raw_parts(input.data() + pair_begin,
                                                   separator - pair_begin);
        auto value_begin = separator < pair_end ? separator + 1 : pair_end;
        auto value_text = ref<str>::from_raw_parts(input.data() + value_begin,
                                                    pair_end - value_begin);

        auto name = form ? decode_form_component(name_text) : decode_component(name_text);
        if (name.is_err()) {
            return Err(rebase(rstd::move(name).unwrap_err(), pair_begin));
        }
        auto value = form ? decode_form_component(value_text) : decode_component(value_text);
        if (value.is_err()) {
            return Err(rebase(rstd::move(value).unwrap_err(), value_begin));
        }
        result.pairs_.push(QueryPair { rstd::move(name).unwrap(),
                                       rstd::move(value).unwrap() });

        if (pair_end == input.size()) break;
        pair_begin = pair_end + 1;
    }
    return Ok(rstd::move(result));
}

void QueryParams::add(ref<str> name, ref<str> value) {
    pairs_.push(QueryPair { String::make(name), String::make(value) });
}

void QueryParams::set(ref<str> name, ref<str> value) {
    (void)remove(name);
    add(name, value);
}

auto QueryParams::get(ref<str> name) const noexcept -> Option<ref<str>> {
    for (auto const& pair : pairs_) {
        if (same(pair.name(), name)) return Some(pair.value());
    }
    return None();
}

auto QueryParams::values(ref<str> name) const noexcept -> QueryValues {
    for (auto const& pair : pairs_) {
        if (same(pair.name(), name)) {
            return QueryValues { pairs_.begin(), pairs_.end(), &pair };
        }
    }
    return QueryValues { pairs_.end(), pairs_.end(), nullptr };
}

auto QueryParams::remove(ref<str> name) noexcept -> usize {
    usize removed = 0;
    for (usize offset = 0; offset < pairs_.len();) {
        if (same(pairs_[offset].name(), name)) {
            pairs_.remove(offset);
            ++removed;
        } else {
            ++offset;
        }
    }
    return removed;
}

auto QueryParams::len() const noexcept -> usize { return pairs_.len(); }

auto QueryParams::is_empty() const noexcept -> bool { return pairs_.is_empty(); }

auto QueryParams::iter() const noexcept -> QueryIter {
    return QueryIter { pairs_.begin(), pairs_.end() };
}

auto QueryParams::encode_query() const -> String { return encode_with_mode(false); }

auto QueryParams::encode_form() const -> String { return encode_with_mode(true); }

auto QueryParams::encode_with_mode(bool form) const -> String {
    auto output = String::make();
    for (usize offset = 0; offset < pairs_.len(); ++offset) {
        if (offset != 0) output.push_back('&');
        auto name = form ? encode_form_component(pairs_[offset].name())
                         : encode_component(pairs_[offset].name());
        auto value = form ? encode_form_component(pairs_[offset].value())
                          : encode_component(pairs_[offset].value());
        output.push_str(name.as_str());
        output.push_back('=');
        output.push_str(value.as_str());
    }
    return output;
}

auto QueryParams::clone() const -> QueryParams {
    auto output   = QueryParams {};
    output.pairs_ = pairs_.clone();
    return output;
}

auto encode_component(ref<str> input) -> String { return encode(input, false); }

auto decode_component(ref<str> input) -> rstd::Result<String, QueryError> {
    return decode(input, false);
}

auto encode_form_component(ref<str> input) -> String { return encode(input, true); }

auto decode_form_component(ref<str> input) -> rstd::Result<String, QueryError> {
    return decode(input, true);
}

} // namespace ncrequest::http
