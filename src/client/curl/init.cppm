export module ncrequest.curl:init;
export import :curl;
export import rstd.core;

using namespace rstd::prelude;

namespace ncrequest
{
export auto curl_init() -> rstd::Result<empty, curl::CURLcode>;
} // namespace ncrequest
