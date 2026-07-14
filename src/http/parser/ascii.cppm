export module ncrequest.http.parser.ascii;
export import ncrequest.http.parser.cursor;

export namespace ncrequest::http::parser::ascii
{

using rstd::u8;

constexpr auto alpha(u8 value) noexcept -> bool {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

constexpr auto digit(u8 value) noexcept -> bool { return value >= '0' && value <= '9'; }

constexpr auto hexdig(u8 value) noexcept -> bool {
    return digit(value) || (value >= 'A' && value <= 'F') ||
           (value >= 'a' && value <= 'f');
}

constexpr auto unreserved(u8 value) noexcept -> bool {
    return alpha(value) || digit(value) || value == '-' || value == '.' || value == '_' ||
           value == '~';
}

constexpr auto sub_delim(u8 value) noexcept -> bool {
    switch (value) {
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
    return alpha(value) || digit(value) || value == '!' || value == '#' || value == '$' ||
           value == '%' || value == '&' || value == '\'' || value == '*' || value == '+' ||
           value == '-' || value == '.' || value == '^' || value == '_' || value == '`' ||
           value == '|' || value == '~';
}

} // namespace ncrequest::http::parser::ascii
