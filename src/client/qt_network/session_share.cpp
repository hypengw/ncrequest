module;
#include <array>
#include <charconv>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

module ncrequest;
import :qt;
import :session_share_backend;

namespace ncrequest
{

using namespace ncrequest::qt;

namespace
{

constexpr std::string_view HttpOnlyPrefix { "#HttpOnly_" };

auto cookie_fields(std::string_view line) -> rstd::Option<std::array<std::string_view, 7>> {
    auto fields = std::array<std::string_view, 7> {};
    for (usize i = 0; i < fields.size() - 1; ++i) {
        auto tab = line.find('\t');
        if (tab == std::string_view::npos) return None<std::array<std::string_view, 7>>();
        fields[i] = line.substr(0, tab);
        line.remove_prefix(tab + 1);
    }
    fields.back() = line;
    return Some(fields);
}

auto parse_cookie(std::string_view line) -> rstd::Option<QNetworkCookie> {
    bool http_only = false;
    if (line.starts_with(HttpOnlyPrefix)) {
        http_only = true;
        line.remove_prefix(HttpOnlyPrefix.size());
    } else if (line.empty() || line.front() == '#') {
        return None<QNetworkCookie>();
    }

    auto parsed = cookie_fields(line);
    if (parsed.is_none()) return None<QNetworkCookie>();
    auto fields = rstd::move(parsed).unwrap();
    if (fields[0].empty() || fields[2].empty() || fields[5].empty()) {
        return None<QNetworkCookie>();
    }

    auto domain = std::string(fields[0]);
    if (fields[1] == "TRUE" && ! domain.starts_with('.')) {
        domain.insert(domain.begin(), '.');
    } else if (fields[1] != "TRUE" && domain.starts_with('.')) {
        domain.erase(domain.begin());
    }

    qint64 expiry = 0;
    auto   expiry_result =
        std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), expiry);
    if (expiry_result.ec != std::errc {} ||
        expiry_result.ptr != fields[4].data() + fields[4].size()) {
        return None<QNetworkCookie>();
    }

    auto cookie =
        QNetworkCookie { QByteArray(fields[5].data(), static_cast<qsizetype>(fields[5].size())),
                         QByteArray(fields[6].data(), static_cast<qsizetype>(fields[6].size())) };
    cookie.setDomain(QString::fromUtf8(domain.data(), static_cast<qsizetype>(domain.size())));
    cookie.setPath(QString::fromUtf8(fields[2].data(), static_cast<qsizetype>(fields[2].size())));
    cookie.setSecure(fields[3] == "TRUE");
    cookie.setHttpOnly(http_only);
    if (expiry > 0) {
        auto expiration = QDateTime {};
        expiration.setSecsSinceEpoch(expiry);
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

} // namespace

class SessionShare::Private {
public:
    class CookieJar final : public QNetworkCookieJar {
    public:
        CookieJar(Weak<Private> state, QObject* parent)
            : QNetworkCookieJar(parent), m_state(rstd::move(state)) {}

        auto cookiesForUrl(const QUrl& url) const -> QList<QNetworkCookie> override {
            auto state = m_state.lock();
            if (! state) return {};

            auto lock = std::lock_guard { state->mutex };
            auto self = const_cast<CookieJar*>(this);
            self->setAllCookies(state->cookies);
            auto cookies   = self->QNetworkCookieJar::cookiesForUrl(url);
            state->cookies = self->allCookies();
            return cookies;
        }

        auto setCookiesFromUrl(const QList<QNetworkCookie>& cookies, const QUrl& url)
            -> bool override {
            auto state = m_state.lock();
            if (! state) return false;

            auto lock = std::lock_guard { state->mutex };
            setAllCookies(state->cookies);
            auto changed   = QNetworkCookieJar::setCookiesFromUrl(cookies, url);
            state->cookies = allCookies();
            return changed;
        }

    private:
        Weak<Private> m_state;
    };

    mutable std::mutex    mutex;
    QList<QNetworkCookie> cookies;
};

SessionShare::SessionShare(): d_ptr(make_arc<Private>()) {}

SessionShare::~SessionShare() = default;

auto SessionShare::clone() const -> SessionShare { return *this; }

void SessionShare::load(const std::filesystem::path& path) {
    auto input = std::ifstream { path, std::ios::binary };
    if (! input) return;

    auto loaded = QList<QNetworkCookie> {};
    auto line   = std::string {};
    auto now    = QDateTime::currentDateTimeUtc();
    while (std::getline(input, line)) {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        auto parsed = parse_cookie(line);
        if (parsed.is_none()) continue;
        auto cookie = rstd::move(parsed).unwrap();
        if (! is_expired(cookie, now)) merge_cookie(loaded, cookie);
    }

    auto lock = std::lock_guard { d_ptr->mutex };
    for (auto const& cookie : loaded) merge_cookie(d_ptr->cookies, cookie);
}

void SessionShare::save(const std::filesystem::path& path) const {
    auto output = std::ofstream { path, std::ios::binary | std::ios::trunc };
    if (! output) return;

    auto cookies = QList<QNetworkCookie> {};
    {
        auto lock = std::lock_guard { d_ptr->mutex };
        cookies   = d_ptr->cookies;
    }

    output << "# Netscape HTTP Cookie File\n# This file was generated by ncrequest.\n\n";
    auto now = QDateTime::currentDateTimeUtc();
    for (auto const& cookie : cookies) {
        if (is_expired(cookie, now)) continue;

        auto domain = cookie.domain().toUtf8();
        auto path   = cookie.path().toUtf8();
        auto name   = cookie.name();
        auto value  = cookie.value();
        if (domain.isEmpty() || name.isEmpty()) continue;

        if (cookie.isHttpOnly()) output << HttpOnlyPrefix;
        output.write(domain.constData(), domain.size());
        output << '\t' << (cookie.domain().startsWith('.') ? "TRUE" : "FALSE") << '\t';
        if (path.isEmpty()) {
            output << '/';
        } else {
            output.write(path.constData(), path.size());
        }
        output << '\t' << (cookie.isSecure() ? "TRUE" : "FALSE") << '\t';
        auto expiry = cookie.expirationDate();
        output << (expiry.isValid() ? expiry.toSecsSinceEpoch() : 0) << '\t';
        output.write(name.constData(), name.size());
        output << '\t';
        output.write(value.constData(), value.size());
        output << '\n';
    }
}

auto detail::SessionShareAccess::token(const SessionShare& share) -> std::shared_ptr<void> {
    return share.d_ptr;
}

auto detail::SessionShareAccess::make_cookie_jar(const SessionShare& share, QObject* parent)
    -> QNetworkCookieJar* {
    return new SessionShare::Private::CookieJar(Weak<SessionShare::Private>(share.d_ptr), parent);
}

} // namespace ncrequest
