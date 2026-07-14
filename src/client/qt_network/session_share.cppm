export module ncrequest:session_share_backend;
export import :qt;
import :session_share;

namespace ncrequest::detail
{

using namespace ncrequest::qt;

export class SessionShareAccess {
public:
    static auto token(const SessionShare&) -> const void*;
    static auto make_cookie_jar(const SessionShare&, QObject*) -> QNetworkCookieJar*;
    static auto cookie_jar_expired(const QNetworkCookieJar*) -> bool;
};

} // namespace ncrequest::detail
