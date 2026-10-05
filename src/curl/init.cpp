module ncrequest.curl;
import :init;
import rstd.core;

using namespace rstd::prelude;

auto ncrequest::curl_init() -> rstd::Result<empty, curl::CURLcode> {
    static const auto code = curl::curl_global_init(curl::CURL_GLOBAL_ALL);
    if (code != curl::CURLcode::CURLE_OK) return Err(code);
    return Ok(empty {});
}
