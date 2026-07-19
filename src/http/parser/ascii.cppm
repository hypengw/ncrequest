export module ncrequest.http.parser.ascii;
export import ncrequest.http.parser.cursor;

export namespace ncrequest::http::parser::ascii
{

using rstd::u8;

constexpr auto alpha(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return (raw >= 'A' && raw <= 'Z') || (raw >= 'a' && raw <= 'z');
}

constexpr auto digit(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return raw >= '0' && raw <= '9';
}

constexpr auto hexdig(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return digit(value) || (raw >= 'A' && raw <= 'F') || (raw >= 'a' && raw <= 'f');
}

constexpr auto unreserved(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return alpha(value) || digit(value) || raw == '-' || raw == '.' || raw == '_' || raw == '~';
}

constexpr auto sub_delim(u8 value) noexcept -> bool {
    switch (value.to_primitive()) {
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=': return true;
    default: return false;
    }
}

constexpr auto tchar(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return alpha(value) || digit(value) || raw == '!' || raw == '#' || raw == '$' || raw == '%' ||
           raw == '&' || raw == '\'' || raw == '*' || raw == '+' || raw == '-' || raw == '.' ||
           raw == '^' || raw == '_' || raw == '`' || raw == '|' || raw == '~';
}

} // namespace ncrequest::http::parser::ascii
