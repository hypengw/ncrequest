export module ncrequest:session_share_backend;
import :session_share;
import ncrequest.curl;

namespace ncrequest::detail
{

export class SessionShareAccess {
public:
    static auto curl_handle(const SessionShare&) -> curl::CURLSH*;
};

} // namespace ncrequest::detail
