module ncrequest;
import :http_header;
import ncrequest.http.parser.ascii;

namespace ncrequest::http
{

namespace
{

constexpr auto ascii_lower(u8 value) noexcept -> u8 {
    auto raw = value.to_primitive();
    if (raw >= 'A' && raw <= 'Z') return u8(raw + ('a' - 'A'));
    return value;
}

constexpr auto valid_value_byte(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return raw == '\t' || (raw >= 0x20 && raw <= 0x7e) || raw >= 0x80;
}

} // namespace

HeaderName::HeaderName(String value) noexcept: value_(rstd::move(value)) {}

auto HeaderName::parse(ref<str> input) -> rstd::Result<HeaderName, HeaderError> {
    if (input.size() == usize()) {
        return Err(HeaderError { HeaderErrorKind::InvalidName(), usize() });
    }

    for (usize offset {}; offset < input.size(); ++offset) {
        if (! parser::ascii::tchar(input[offset])) {
            return Err(HeaderError { HeaderErrorKind::InvalidName(), offset });
        }
    }
    return Ok(HeaderName { String::make(input) });
}

auto HeaderName::as_ref() const noexcept -> ref<str> { return value_.as_str(); }

auto HeaderName::equals(ref<str> other) const noexcept -> bool {
    auto self = as_ref();
    if (self.size() != other.size()) return false;
    for (usize offset {}; offset < self.size(); ++offset) {
        if (ascii_lower(self[offset]) != ascii_lower(other[offset])) return false;
    }
    return true;
}

auto HeaderName::clone() const -> HeaderName { return HeaderName { value_.clone() }; }

HeaderValue::HeaderValue(Vec<u8> value) noexcept: value_(rstd::move(value)) {}

auto HeaderValue::parse(ref<str> input) -> rstd::Result<HeaderValue, HeaderError> {
    return from_bytes(input.as_bytes());
}

auto HeaderValue::from_bytes(slice<u8> input) -> rstd::Result<HeaderValue, HeaderError> {
    for (usize offset {}; offset < input.len(); ++offset) {
        auto value = input[offset];
        if (! valid_value_byte(value)) {
            auto raw  = value.to_primitive();
            auto kind = raw == '\r' || raw == '\n' ? HeaderErrorKind::InvalidLineBreak()
                                                   : HeaderErrorKind::InvalidValue();
            return Err(HeaderError { rstd::move(kind), offset });
        }
    }
    return Ok(HeaderValue { Vec<u8>::from(input) });
}

auto HeaderValue::as_bytes() const noexcept -> slice<u8> { return value_.as_slice(); }

auto HeaderValue::as_str() const noexcept -> Option<ref<str>> {
    return rstd::str_::from_utf8(as_bytes()).ok();
}

auto HeaderValue::clone() const -> HeaderValue { return HeaderValue { value_.clone() }; }

HeaderField::HeaderField(HeaderName name, HeaderValue value) noexcept
    : name_(rstd::move(name)), value_(rstd::move(value)) {}

auto HeaderField::name() const noexcept -> const HeaderName& { return name_; }

auto HeaderField::value() const noexcept -> const HeaderValue& { return value_; }

auto HeaderField::clone() const -> HeaderField {
    return HeaderField { name_.clone(), value_.clone() };
}

HeaderIter::HeaderIter(const HeaderField* current, const HeaderField* end) noexcept
    : current_(current), end_(end) {}

auto HeaderIter::next() noexcept -> Option<Item> {
    if (current_ == end_) return None();
    auto* value = current_++;
    return Some(ref<HeaderField>::from_raw_parts(value));
}

HeaderValues::HeaderValues(const HeaderField* current, const HeaderField* end,
                           const HeaderName* name) noexcept
    : current_(current), end_(end), name_(name) {}

auto HeaderValues::next() noexcept -> Option<Item> {
    if (name_ == nullptr) return None();
    while (current_ != end_) {
        auto* field = current_++;
        if (field->name().equals(name_->as_ref())) {
            return Some(ref<HeaderValue>::from_raw_parts(&field->value()));
        }
    }
    return None();
}

auto Header::add(ref<str> name, HeaderValue value) -> rstd::Result<empty, HeaderError> {
    auto parsed_name = HeaderName::parse(name);
    if (parsed_name.is_err()) return Err(rstd::move(parsed_name).unwrap_err());
    fields_.push(HeaderField { rstd::move(parsed_name).unwrap(), rstd::move(value) });
    return Ok(empty {});
}

auto Header::add(ref<str> name, ref<str> value) -> rstd::Result<empty, HeaderError> {
    auto parsed_value = HeaderValue::parse(value);
    if (parsed_value.is_err()) return Err(rstd::move(parsed_value).unwrap_err());
    return add(name, rstd::move(parsed_value).unwrap());
}

auto Header::set(ref<str> name, HeaderValue value) -> rstd::Result<empty, HeaderError> {
    auto parsed_name = HeaderName::parse(name);
    if (parsed_name.is_err()) return Err(rstd::move(parsed_name).unwrap_err());
    auto owned_name = rstd::move(parsed_name).unwrap();
    (void)remove(owned_name.as_ref());
    fields_.push(HeaderField { rstd::move(owned_name), rstd::move(value) });
    return Ok(empty {});
}

auto Header::set(ref<str> name, ref<str> value) -> rstd::Result<empty, HeaderError> {
    auto parsed_value = HeaderValue::parse(value);
    if (parsed_value.is_err()) return Err(rstd::move(parsed_value).unwrap_err());
    return set(name, rstd::move(parsed_value).unwrap());
}

void Header::append(HeaderField field) { fields_.push(rstd::move(field)); }

auto Header::get(ref<str> name) const noexcept -> Option<ref<HeaderValue>> {
    for (auto const& field : fields_) {
        if (field.name().equals(name)) {
            return Some(ref<HeaderValue>::from_raw_parts(&field.value()));
        }
    }
    return None();
}

auto Header::values(ref<str> name) const noexcept -> HeaderValues {
    for (auto const& field : fields_) {
        if (field.name().equals(name)) {
            return HeaderValues { fields_.begin(), fields_.end(), &field.name() };
        }
    }
    return HeaderValues { fields_.end(), fields_.end(), nullptr };
}

auto Header::contains(ref<str> name) const noexcept -> bool { return get(name).is_some(); }

auto Header::has_field(ref<str> name) const noexcept -> bool { return contains(name); }

auto Header::remove(ref<str> name) noexcept -> usize {
    usize removed {};
    for (usize offset {}; offset < fields_.len();) {
        if (fields_[offset].name().equals(name)) {
            fields_.remove(offset);
            ++removed;
        } else {
            ++offset;
        }
    }
    return removed;
}

auto Header::len() const noexcept -> usize { return fields_.len(); }

auto Header::is_empty() const noexcept -> bool { return fields_.is_empty(); }

auto Header::iter() const noexcept -> HeaderIter {
    return HeaderIter { fields_.begin(), fields_.end() };
}

auto Header::clone() const -> Header {
    auto result    = Header {};
    result.fields_ = fields_.clone();
    return result;
}

} // namespace ncrequest::http
