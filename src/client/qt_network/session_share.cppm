module;
#include <memory>
#include <QNetworkCookieJar>
#include <QObject>

export module ncrequest:session_share_backend;
import :session_share;

namespace ncrequest::detail
{

export class SessionShareAccess {
public:
    static auto token(const SessionShare&) -> std::shared_ptr<void>;
    static auto make_cookie_jar(const SessionShare&, QObject*) -> QNetworkCookieJar*;
};

} // namespace ncrequest::detail
