module;
module ncrequest;
import :qt;
import :session_share_backend;

namespace ncrequest
{

using namespace ncrequest::qt;
using namespace rstd::literals;

namespace
{

constexpr auto HttpOnlyPrefix = "#HttpOnly_"_bytes;

struct CookieFields {
    slice<u8> values[7] {};

    auto operator[](usize index) const -> slice<u8> { return values[index.to_primitive()]; }
};

auto starts_with(slice<u8> value, slice<u8> prefix) -> bool {
    if (prefix.len() > value.len()) return false;
    return __builtin_memcmp(value.as_raw_ptr(), prefix.as_raw_ptr(), prefix.len().to_primitive()) ==
           0;
}

auto equals(slice<u8> value, ref<str> expected) -> bool {
    return value.len() == expected.size() &&
           __builtin_memcmp(value.as_raw_ptr(), expected.data(), value.len().to_primitive()) == 0;
}

auto cookie_fields(slice<u8> line) -> rstd::Option<CookieFields> {
    auto fields = CookieFields {};
    for (usize i {}; i < usize(6); ++i) {
        auto tab = line.len();
        for (usize j {}; j < line.len(); ++j) {
            if (line[j] == u8('\t')) {
                tab = j;
                break;
            }
        }
        if (tab == line.len()) return None<CookieFields>();
        fields.values[i.to_primitive()] = slice<u8>::from_raw_parts(line.as_raw_ptr(), tab);
        line = slice<u8>::from_raw_parts(line.as_raw_ptr() + tab.to_primitive() + 1,
                                         line.len() - tab - usize(1));
    }
    fields.values[6] = line;
    return Some(rstd::move(fields));
}

auto parse_expiry(slice<u8> text) -> Option<qint64> {
    if (text.len() == usize()) return None<qint64>();
    qint64 value = 0;
    for (usize i {}; i < text.len(); ++i) {
        auto raw = text[i].to_primitive();
        if (raw < '0' || raw > '9') return None<qint64>();
        auto digit = static_cast<qint64>(raw - '0');
        if (value > (std::numeric_limits<qint64>::max() - digit) / 10) return None<qint64>();
        value = value * 10 + digit;
    }
    return Some(value);
}

auto parse_cookie(slice<u8> line) -> rstd::Option<QNetworkCookie> {
    bool http_only = false;
    if (starts_with(line, HttpOnlyPrefix)) {
        http_only = true;
        line = slice<u8>::from_raw_parts(line.as_raw_ptr() + HttpOnlyPrefix.len().to_primitive(),
                                         line.len() - HttpOnlyPrefix.len());
    } else if (line.len() == usize() || line[usize()] == u8('#')) {
        return None<QNetworkCookie>();
    }

    auto parsed = cookie_fields(line);
    if (parsed.is_none()) return None<QNetworkCookie>();
    auto fields = rstd::move(parsed).unwrap();
    if (fields[usize()].len() == usize() || fields[usize(2)].len() == usize() ||
        fields[usize(5)].len() == usize()) {
        return None<QNetworkCookie>();
    }

    auto domain = QByteArray(reinterpret_cast<const char*>(fields[usize()].as_raw_ptr()),
                             static_cast<qsizetype>(fields[usize()].len().to_primitive()));
    if (equals(fields[usize(1)], "TRUE"_str) && ! domain.startsWith('.')) {
        domain.prepend('.');
    } else if (! equals(fields[usize(1)], "TRUE"_str) && domain.startsWith('.')) {
        domain.remove(0, 1);
    }

    auto expiry = parse_expiry(fields[usize(4)]);
    if (expiry.is_none()) return None<QNetworkCookie>();

    auto cookie = QNetworkCookie {
        QByteArray(reinterpret_cast<const char*>(fields[usize(5)].as_raw_ptr()),
                   static_cast<qsizetype>(fields[usize(5)].len().to_primitive())),
        QByteArray(reinterpret_cast<const char*>(fields[usize(6)].as_raw_ptr()),
                   static_cast<qsizetype>(fields[usize(6)].len().to_primitive()))
    };
    cookie.setDomain(QString::fromUtf8(domain));
    cookie.setPath(
        QString::fromUtf8(reinterpret_cast<const char*>(fields[usize(2)].as_raw_ptr()),
                          static_cast<qsizetype>(fields[usize(2)].len().to_primitive())));
    cookie.setSecure(equals(fields[usize(3)], "TRUE"_str));
    cookie.setHttpOnly(http_only);
    if (*expiry > 0) {
        auto expiration = QDateTime {};
        expiration.setSecsSinceEpoch(*expiry);
        cookie.setExpirationDate(expiration);
    }
    return Some(rstd::move(cookie));
}

void merge_cookie(QList<QNetworkCookie>& cookies, const QNetworkCookie& cookie) {
    for (auto& existing : cookies) {
        if (existing.hasSameIdentifier(cookie)) {
            existing = cookie;
            return;
        }
    }
    cookies.push_back(cookie);
}

auto is_expired(const QNetworkCookie& cookie, const QDateTime& now) -> bool {
    auto expiry = cookie.expirationDate();
    return expiry.isValid() && expiry <= now;
}

void append_text(rstd::vec::Vec<u8>& output, ref<str> text) {
    output.extend_from_slice(text.as_bytes());
}

void append_bytes(rstd::vec::Vec<u8>& output, const QByteArray& bytes) {
    output.extend_from_slice(slice<u8>::from_raw_parts(
        reinterpret_cast<const byte*>(bytes.constData()), static_cast<usize>(bytes.size())));
}

} // namespace

class SessionShare::Private {
public:
    Private(): cookies(QList<QNetworkCookie> {}) {}

    class CookieJar final : public QNetworkCookieJar {
    public:
        CookieJar(Weak<Private> state, QObject* parent)
            : QNetworkCookieJar(parent), m_state(rstd::move(state)) {}

        auto cookiesForUrl(const QUrl& url) const -> QList<QNetworkCookie> override {
            auto state = m_state.upgrade();
            if (! state) return {};

            auto cookies = state->cookies.lock().unwrap();
            auto self    = const_cast<CookieJar*>(this);
            self->setAllCookies(*cookies);
            auto selected = self->QNetworkCookieJar::cookiesForUrl(url);
            *cookies      = self->allCookies();
            return selected;
        }

        auto setCookiesFromUrl(const QList<QNetworkCookie>& cookies, const QUrl& url)
            -> bool override {
            auto state = m_state.upgrade();
            if (! state) return false;

            auto stored = state->cookies.lock().unwrap();
            setAllCookies(*stored);
            auto changed = QNetworkCookieJar::setCookiesFromUrl(cookies, url);
            *stored      = allCookies();
            return changed;
        }

        auto expired() const noexcept -> bool { return m_state.expired(); }

    private:
        Weak<Private> m_state;
    };

    rstd::sync::Mutex<QList<QNetworkCookie>> cookies;
};

SessionShare::SessionShare(Arc<Private> state): d_ptr(rstd::move(state)) {}
SessionShare::SessionShare(SessionShare&&) noexcept                    = default;
auto SessionShare::operator=(SessionShare&&) noexcept -> SessionShare& = default;

SessionShare::SessionShare(): d_ptr(Arc<Private>::make()) {}

SessionShare::~SessionShare() = default;

auto SessionShare::clone() const -> SessionShare { return SessionShare { d_ptr.clone() }; }

void SessionShare::load(ref<rstd::path::Path> path) {
    auto input = rstd::fs::read(path);
    if (input.is_err()) return;

    auto  loaded = QList<QNetworkCookie> {};
    auto  bytes  = rstd::move(input).unwrap();
    auto  values = bytes.as_slice();
    auto  now    = QDateTime::currentDateTimeUtc();
    usize begin {};
    while (begin <= bytes.len()) {
        auto newline = begin;
        while (newline < bytes.len() && values[newline].to_primitive() != '\n') ++newline;
        auto end = newline;
        if (end > begin && values[end - usize(1)].to_primitive() == '\r') --end;
        auto line   = slice<u8>::from_raw_parts(bytes.data() + begin.to_primitive(), end - begin);
        auto parsed = parse_cookie(line);
        if (parsed.is_some()) {
            auto cookie = rstd::move(parsed).unwrap();
            if (! is_expired(cookie, now)) merge_cookie(loaded, cookie);
        }
        if (newline == bytes.len()) break;
        begin = newline + usize(1);
    }

    auto cookies = d_ptr->cookies.lock().unwrap();
    for (auto const& cookie : loaded) merge_cookie(*cookies, cookie);
}

void SessionShare::save(ref<rstd::path::Path> path) const {
    auto cookies = QList<QNetworkCookie> {};
    {
        auto stored = d_ptr->cookies.lock().unwrap();
        cookies     = *stored;
    }

    auto output = rstd::vec::Vec<u8>::make();
    append_text(output,
                "# Netscape HTTP Cookie File\n# This file was generated by ncrequest.\n\n"_str);
    auto now = QDateTime::currentDateTimeUtc();
    for (auto const& cookie : cookies) {
        if (is_expired(cookie, now)) continue;

        auto domain = cookie.domain().toUtf8();
        auto path   = cookie.path().toUtf8();
        auto name   = cookie.name();
        auto value  = cookie.value();
        if (domain.isEmpty() || name.isEmpty()) continue;

        if (cookie.isHttpOnly()) output.extend_from_slice(HttpOnlyPrefix);
        append_bytes(output, domain);
        append_text(output, cookie.domain().startsWith('.') ? "\tTRUE\t"_str : "\tFALSE\t"_str);
        if (path.isEmpty()) {
            append_text(output, "/"_str);
        } else {
            append_bytes(output, path);
        }
        append_text(output, cookie.isSecure() ? "\tTRUE\t"_str : "\tFALSE\t"_str);
        auto expiry      = cookie.expirationDate();
        auto expiry_text = rstd::format("{}", expiry.isValid() ? expiry.toSecsSinceEpoch() : 0);
        append_text(output, expiry_text.as_str());
        append_text(output, "\t"_str);
        append_bytes(output, name);
        append_text(output, "\t"_str);
        append_bytes(output, value);
        append_text(output, "\n"_str);
    }
    (void)rstd::fs::write(path, output.as_slice());
}

auto detail::SessionShareAccess::token(const SessionShare& share) -> const void* {
    return share.d_ptr.as_ptr().as_raw_ptr();
}

auto detail::SessionShareAccess::make_cookie_jar(const SessionShare& share, QObject* parent)
    -> QNetworkCookieJar* {
    return new SessionShare::Private::CookieJar(share.d_ptr.downgrade(), parent);
}

auto detail::SessionShareAccess::cookie_jar_expired(const QNetworkCookieJar* jar) -> bool {
    auto* share_jar = dynamic_cast<const SessionShare::Private::CookieJar*>(jar);
    return share_jar == nullptr || share_jar->expired();
}

} // namespace ncrequest
