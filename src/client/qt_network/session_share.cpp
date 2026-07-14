module;
module ncrequest;
import :qt;
import :session_share_backend;

namespace ncrequest
{

using namespace ncrequest::qt;

namespace
{

const auto HttpOnlyPrefix = rstd::str_::as_bytes("#HttpOnly_");

struct CookieFields {
    slice<u8> values[7] {};

    auto operator[](usize index) const -> slice<u8> { return values[index]; }
};

auto starts_with(slice<u8> value, slice<u8> prefix) -> bool {
    if (prefix.len() > value.len()) return false;
    return __builtin_memcmp(value.as_raw_ptr(), prefix.as_raw_ptr(), prefix.len()) == 0;
}

auto equals(slice<u8> value, ref<str> expected) -> bool {
    return value.len() == expected.size() &&
           __builtin_memcmp(value.as_raw_ptr(), expected.data(), value.len()) == 0;
}

auto cookie_fields(slice<u8> line) -> rstd::Option<CookieFields> {
    auto fields = CookieFields {};
    for (usize i = 0; i < 6; ++i) {
        auto tab = line.len();
        for (usize j = 0; j < line.len(); ++j) {
            if (line[j] == '\t') {
                tab = j;
                break;
            }
        }
        if (tab == line.len()) return None<CookieFields>();
        fields.values[i] = slice<u8>::from_raw_parts(line.as_raw_ptr(), tab);
        line = slice<u8>::from_raw_parts(line.as_raw_ptr() + tab + 1, line.len() - tab - 1);
    }
    fields.values[6] = line;
    return Some(rstd::move(fields));
}

auto parse_expiry(slice<u8> text) -> Option<qint64> {
    if (text.len() == 0) return None<qint64>();
    qint64 value = 0;
    for (usize i = 0; i < text.len(); ++i) {
        auto byte = text[i];
        if (byte < '0' || byte > '9') return None<qint64>();
        auto digit = static_cast<qint64>(byte - '0');
        if (value > (numeric_limits<qint64>::max() - digit) / 10) return None<qint64>();
        value = value * 10 + digit;
    }
    return Some(value);
}

auto parse_cookie(slice<u8> line) -> rstd::Option<QNetworkCookie> {
    bool http_only = false;
    if (starts_with(line, HttpOnlyPrefix)) {
        http_only = true;
        line = slice<u8>::from_raw_parts(line.as_raw_ptr() + HttpOnlyPrefix.len(),
                                         line.len() - HttpOnlyPrefix.len());
    } else if (line.len() == 0 || line[0] == '#') {
        return None<QNetworkCookie>();
    }

    auto parsed = cookie_fields(line);
    if (parsed.is_none()) return None<QNetworkCookie>();
    auto fields = rstd::move(parsed).unwrap();
    if (fields[0].len() == 0 || fields[2].len() == 0 || fields[5].len() == 0) {
        return None<QNetworkCookie>();
    }

    auto domain = QByteArray(reinterpret_cast<const char*>(fields[0].as_raw_ptr()),
                             static_cast<qsizetype>(fields[0].len()));
    if (equals(fields[1], "TRUE") && ! domain.startsWith('.')) {
        domain.prepend('.');
    } else if (! equals(fields[1], "TRUE") && domain.startsWith('.')) {
        domain.remove(0, 1);
    }

    auto expiry = parse_expiry(fields[4]);
    if (expiry.is_none()) return None<QNetworkCookie>();

    auto cookie = QNetworkCookie {
        QByteArray(reinterpret_cast<const char*>(fields[5].as_raw_ptr()),
                   static_cast<qsizetype>(fields[5].len())),
        QByteArray(reinterpret_cast<const char*>(fields[6].as_raw_ptr()),
                   static_cast<qsizetype>(fields[6].len())) };
    cookie.setDomain(QString::fromUtf8(domain));
    cookie.setPath(QString::fromUtf8(reinterpret_cast<const char*>(fields[2].as_raw_ptr()),
                                     static_cast<qsizetype>(fields[2].len())));
    cookie.setSecure(equals(fields[3], "TRUE"));
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

auto rstd_path(const std::filesystem::path& path) -> rstd::path::PathBuf {
    auto text = path.string();
    return rstd::path::PathBuf::from(ref<str>(text));
}

void append_text(rstd::vec::Vec<u8>& output, ref<str> text) {
    output.extend_from_slice(rstd::str_::as_bytes(text));
}

void append_bytes(rstd::vec::Vec<u8>& output, const QByteArray& bytes) {
    output.extend_from_slice(slice<u8>::from_raw_parts(
        reinterpret_cast<const u8*>(bytes.constData()), static_cast<usize>(bytes.size())));
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
            auto self = const_cast<CookieJar*>(this);
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
SessionShare::SessionShare(SessionShare&&) noexcept = default;
auto SessionShare::operator=(SessionShare&&) noexcept -> SessionShare& = default;

SessionShare::SessionShare(): d_ptr(Arc<Private>::make()) {}

SessionShare::~SessionShare() = default;

auto SessionShare::clone() const -> SessionShare { return SessionShare { d_ptr.clone() }; }

void SessionShare::load(const std::filesystem::path& path) {
    auto input = rstd::fs::read(rstd_path(path).as_path());
    if (input.is_err()) return;

    auto loaded = QList<QNetworkCookie> {};
    auto bytes  = rstd::move(input).unwrap();
    auto now    = QDateTime::currentDateTimeUtc();
    usize begin = 0;
    while (begin <= bytes.len()) {
        auto newline = begin;
        while (newline < bytes.len() && bytes[newline] != '\n') ++newline;
        auto end = newline;
        if (end > begin && bytes[end - 1] == '\r') --end;
        auto line = slice<u8>::from_raw_parts(bytes.data() + begin, end - begin);
        auto parsed = parse_cookie(line);
        if (parsed.is_some()) {
            auto cookie = rstd::move(parsed).unwrap();
            if (! is_expired(cookie, now)) merge_cookie(loaded, cookie);
        }
        if (newline == bytes.len()) break;
        begin = newline + 1;
    }

    auto cookies = d_ptr->cookies.lock().unwrap();
    for (auto const& cookie : loaded) merge_cookie(*cookies, cookie);
}

void SessionShare::save(const std::filesystem::path& path) const {
    auto cookies = QList<QNetworkCookie> {};
    {
        auto stored = d_ptr->cookies.lock().unwrap();
        cookies     = *stored;
    }

    auto output = rstd::vec::Vec<u8>::make();
    append_text(output, "# Netscape HTTP Cookie File\n# This file was generated by ncrequest.\n\n");
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
        append_text(output, cookie.domain().startsWith('.') ? "\tTRUE\t" : "\tFALSE\t");
        if (path.isEmpty()) {
            append_text(output, "/");
        } else {
            append_bytes(output, path);
        }
        append_text(output, cookie.isSecure() ? "\tTRUE\t" : "\tFALSE\t");
        auto expiry = cookie.expirationDate();
        auto expiry_text = rstd::format("{}", expiry.isValid() ? expiry.toSecsSinceEpoch() : 0);
        append_text(output, expiry_text.as_str());
        append_text(output, "\t");
        append_bytes(output, name);
        append_text(output, "\t");
        append_bytes(output, value);
        append_text(output, "\n");
    }
    (void)rstd::fs::write(rstd_path(path).as_path(), output.as_slice());
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
