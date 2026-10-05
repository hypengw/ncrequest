export module ncrequest:client.qt_network.session_share;
export import :client.qt_network.qt;
import :session_share;

using namespace ncrequest::qt;

namespace ncrequest::detail
{

export class SessionShareAccess {
public:
    static auto token(const SessionShare&) -> const void*;
    static auto make_cookie_jar(const SessionShare&, QObject*) -> QNetworkCookieJar*;
    static auto cookie_jar_expired(const QNetworkCookieJar*) -> bool;
};

} // namespace ncrequest::detail
