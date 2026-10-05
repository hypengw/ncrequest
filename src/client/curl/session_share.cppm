export module ncrequest:client.curl.session_share;
import :session_share;
import ncrequest.curl;

namespace ncrequest::detail
{

export class SessionShareAccess {
public:
    static auto curl_handle(const SessionShare&) -> curl::CURLSH*;
};

} // namespace ncrequest::detail
