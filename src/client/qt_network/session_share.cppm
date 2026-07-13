module;
#include <memory>

export module ncrequest:session_share_backend;
export import :qt;
import :session_share;

namespace ncrequest::detail
{

using namespace ncrequest::qt;

export class SessionShareAccess {
public:
    static auto token(const SessionShare&) -> std::shared_ptr<void>;
    static auto make_cookie_jar(const SessionShare&, QObject*) -> QNetworkCookieJar*;
};

} // namespace ncrequest::detail
