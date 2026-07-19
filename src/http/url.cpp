module ncrequest;
import :http_url;
import ncrequest.http.parser;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;

namespace
{

auto equals_ascii_case_insensitive(ref<str> left, const char* right) noexcept -> bool {
    auto right_size = usize(rstd::strlen(right));
    if (left.size() != right_size) return false;

    for (usize i {}; i < left.size(); ++i) {
        auto index = i.to_primitive();
        auto lhs   = static_cast<unsigned char>(left.data()[index]);
        auto rhs   = static_cast<unsigned char>(right[index]);
        if (lhs >= 'A' && lhs <= 'Z') lhs = static_cast<unsigned char>(lhs + ('a' - 'A'));
        if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<unsigned char>(rhs + ('a' - 'A'));
        if (lhs != rhs) return false;
    }
    return true;
}

auto starts_with_at(ref<str> input, usize offset, ref<str> prefix) noexcept -> bool {
    if (offset > input.size() || prefix.size() > input.size() - offset) return false;
    for (usize i {}; i < prefix.size(); ++i) {
        if (input[offset + i] != prefix[i]) return false;
    }
    return true;
}

void append_range(String& output, ref<str> input, usize begin, usize end) {
    output.push_str(ref<str>::from_raw_parts(input.data() + begin.to_primitive(), end - begin));
}

void remove_last_segment(String& output) {
    auto size = output.size();
    while (size > usize()) {
        --size;
        if (output.data()[size.to_primitive()] == '/') {
            output.truncate(size);
            return;
        }
    }
    output.clear();
}

auto remove_dot_segments(ref<str> input) -> String {
    auto  output = String::make();
    usize offset {};

    while (offset < input.size()) {
        if (starts_with_at(input, offset, "../")) {
            offset += usize(3);
            continue;
        }
        if (starts_with_at(input, offset, "./")) {
            offset += usize(2);
            continue;
        }
        if (starts_with_at(input, offset, "/./")) {
            offset += usize(2);
            continue;
        }
        if (starts_with_at(input, offset, "/.") && offset + usize(2) == input.size()) {
            output.push_back('/');
            break;
        }
        if (starts_with_at(input, offset, "/../")) {
            offset += usize(3);
            remove_last_segment(output);
            continue;
        }
        if (starts_with_at(input, offset, "/..") && offset + usize(3) == input.size()) {
            remove_last_segment(output);
            output.push_back('/');
            break;
        }
        if ((starts_with_at(input, offset, ".") && offset + usize(1) == input.size()) ||
            (starts_with_at(input, offset, "..") && offset + usize(2) == input.size())) {
            break;
        }

        auto segment_end = offset;
        if (input[segment_end].to_primitive() == '/') ++segment_end;
        while (segment_end < input.size() && input[segment_end].to_primitive() != '/') {
            ++segment_end;
        }
        append_range(output, input, offset, segment_end);
        offset = segment_end;
    }
    return output;
}

auto merge_paths(const Url& base, ref<str> reference_path) -> String {
    auto merged    = String::make();
    auto base_path = base.path();
    if (base.authority().is_some() && base_path.size() == usize()) {
        merged.push_back('/');
        merged.push_str(reference_path);
        return merged;
    }

    usize prefix = base_path.size();
    while (prefix > usize() && base_path[prefix - usize(1)].to_primitive() != '/') --prefix;
    append_range(merged, base_path, usize(), prefix);
    merged.push_str(reference_path);
    return merged;
}

auto parser_error(parser::ParseFailure failure) -> UrlError {
    auto const& expected = failure.expected();
    if (expected.is_PercentEncoding()) {
        return UrlError { UrlErrorKind::InvalidPercentEncoding(), failure.offset() };
    }
    if (expected.is_IpLiteral() || expected.is_Ipv4Address() || expected.is_Ipv6Address()) {
        return UrlError { UrlErrorKind::InvalidIpAddress(), failure.offset() };
    }
    if (expected.is_Port()) {
        return UrlError { UrlErrorKind::InvalidPort(), failure.offset() };
    }
    if (failure.is_incomplete()) {
        return UrlError { UrlErrorKind::UnexpectedEnd(), failure.offset() };
    }
    if (expected.is_AsciiClass() || expected.is_Host() || expected.is_Path() ||
        expected.is_Query() || expected.is_Fragment()) {
        return UrlError { UrlErrorKind::InvalidCharacter(), failure.offset() };
    }
    return UrlError { UrlErrorKind::InvalidSyntax(), failure.offset() };
}

} // namespace

Url::Url(String source, Component scheme, Component authority, Component userinfo, Component host,
         Component port, Component path, Component query, Component fragment) noexcept
    : source_(rstd::move(source)),
      scheme_(scheme),
      authority_(authority),
      userinfo_(userinfo),
      host_(host),
      port_(port),
      path_(path),
      query_(query),
      fragment_(fragment) {}

auto Url::parse(ref<str> input) -> rstd::Result<Url, UrlError> {
    auto parsed = parser::uri::parse(input);
    if (parsed.is_err()) {
        return Err(parser_error(rstd::move(parsed).unwrap_err()));
    }

    auto parts    = rstd::move(parsed).unwrap();
    auto optional = [](parser::uri::OptionalSpan span) {
        return Component {
            .offset  = span.span.begin,
            .size    = span.span.size(),
            .present = span.present,
        };
    };
    auto path = Component {
        .offset  = parts.path.begin,
        .size    = parts.path.size(),
        .present = true,
    };

    return Ok(Url { String::make(input),
                    optional(parts.scheme),
                    optional(parts.authority),
                    optional(parts.userinfo),
                    optional(parts.host),
                    optional(parts.port),
                    path,
                    optional(parts.query),
                    optional(parts.fragment) });
}

auto Url::parse_http(ref<str> input) -> rstd::Result<Url, UrlError> {
    auto parsed = parse(input);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());

    auto url        = rstd::move(parsed).unwrap();
    auto scheme_ref = url.scheme();
    if (scheme_ref.is_none()) {
        return Err(UrlError { UrlErrorKind::MissingScheme(), usize() });
    }
    if (! equals_ascii_case_insensitive(*scheme_ref, "http") &&
        ! equals_ascii_case_insensitive(*scheme_ref, "https")) {
        return Err(UrlError { UrlErrorKind::UnsupportedScheme(), usize() });
    }
    if (url.authority().is_none()) {
        return Err(UrlError { UrlErrorKind::MissingAuthority(), scheme_ref->size() + usize(1) });
    }
    auto host_ref = url.host();
    if (host_ref.is_none() || host_ref->size() == usize()) {
        auto authority_ref = url.authority();
        auto offset =
            authority_ref.is_some() ? usize(authority_ref->data() - url.as_ref().data()) : usize();
        return Err(UrlError { UrlErrorKind::MissingHost(), offset });
    }

    auto host_value = *host_ref;
    if (host_value.size() > usize() && host_value[usize()].to_primitive() != '[') {
        bool numeric = false;
        bool dotted  = false;
        for (auto value : host_value) {
            if (value == '.') {
                dotted = true;
            } else if (! parser::ascii::digit(rstd::byte_value(value))) {
                numeric = false;
                dotted  = false;
                break;
            } else {
                numeric = true;
            }
        }
        if (numeric && dotted) {
            auto ipv4 = parser::uri::validate_ipv4(host_value);
            if (ipv4.is_err()) {
                auto failure = rstd::move(ipv4).unwrap_err();
                auto offset  = usize(host_value.data() - url.as_ref().data()) + failure.offset();
                return Err(UrlError { UrlErrorKind::InvalidIpAddress(), offset });
            }
        }
    }

    auto port_ref = url.port();
    if (port_ref.is_some() && port_ref->size() > usize()) {
        usize value {};
        for (usize i {}; i < port_ref->size(); ++i) {
            value = value * usize(10) + usize((*port_ref)[i].to_primitive() - '0');
            if (value > usize(65535)) {
                auto offset = usize(port_ref->data() - url.as_ref().data()) + i;
                return Err(UrlError { UrlErrorKind::InvalidPort(), offset });
            }
        }
    }
    return Ok(rstd::move(url));
}

auto Url::as_ref() const noexcept -> ref<str> { return source_.as_str(); }

auto Url::component(Component value) const noexcept -> Option<ref<str>> {
    if (! value.present) return None();
    auto source = source_.as_str();
    return Some(ref<str>::from_raw_parts(source.data() + value.offset.to_primitive(), value.size));
}

auto Url::scheme() const noexcept -> Option<ref<str>> { return component(scheme_); }
auto Url::authority() const noexcept -> Option<ref<str>> { return component(authority_); }
auto Url::userinfo() const noexcept -> Option<ref<str>> { return component(userinfo_); }
auto Url::host() const noexcept -> Option<ref<str>> { return component(host_); }
auto Url::port() const noexcept -> Option<ref<str>> { return component(port_); }

auto Url::path() const noexcept -> ref<str> {
    auto source = source_.as_str();
    return ref<str>::from_raw_parts(source.data() + path_.offset.to_primitive(), path_.size);
}

auto Url::query() const noexcept -> Option<ref<str>> { return component(query_); }
auto Url::fragment() const noexcept -> Option<ref<str>> { return component(fragment_); }

auto Url::request_target() const -> String {
    auto target    = String::make();
    auto path_view = path();
    if (path_view.size() == usize() && authority_.present) {
        target.push_back('/');
    } else {
        target.push_str(path_view);
    }
    auto query_view = query();
    if (query_view.is_some()) {
        target.push_back('?');
        target.push_str(*query_view);
    }
    return target;
}

auto Url::resolve(const Url& reference) const -> rstd::Result<Url, UrlError> {
    auto target = String::make();

    auto target_scheme    = reference.scheme();
    auto target_authority = reference.authority();
    auto target_query     = reference.query();
    auto target_path      = String::make();

    if (target_scheme.is_some()) {
        target_path = remove_dot_segments(reference.path());
    } else {
        target_scheme = scheme();
        if (target_authority.is_some()) {
            target_path = remove_dot_segments(reference.path());
        } else {
            target_authority = authority();
            if (reference.path().size() == usize()) {
                target_path = String::make(path());
                if (target_query.is_none()) target_query = query();
            } else if (reference.path()[usize()].to_primitive() == '/') {
                target_path = remove_dot_segments(reference.path());
            } else {
                auto merged = merge_paths(*this, reference.path());
                target_path = remove_dot_segments(merged.as_str());
            }
        }
    }

    if (target_scheme.is_some()) {
        target.push_str(*target_scheme);
        target.push_back(':');
    }
    if (target_authority.is_some()) {
        target.push_str("//");
        target.push_str(*target_authority);
    }
    target.push_str(target_path.as_str());
    if (target_query.is_some()) {
        target.push_back('?');
        target.push_str(*target_query);
    }
    auto target_fragment = reference.fragment();
    if (target_fragment.is_some()) {
        target.push_back('#');
        target.push_str(*target_fragment);
    }
    return Url::parse(target.as_str());
}

auto Url::clone() const -> Url {
    return Url { source_.clone(), scheme_, authority_, userinfo_, host_,
                 port_,           path_,   query_,     fragment_ };
}

} // namespace ncrequest::http
