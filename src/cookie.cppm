export module ncrequest:cookie;
export import :error;
using namespace rstd::prelude;
export namespace ncrequest
{
using CookieJar      = lifetchto::http::MemoryCookieJar;
using CookieBinding  = lifetchto::http::CookieBinding;
using RequestCookies = lifetchto::http::RequestCookies;
auto load_cookies(const CookieJar& jar, ref<rstd::path::Path> path) -> Result<empty>;
auto save_cookies(const CookieJar& jar, ref<rstd::path::Path> path) -> Result<empty>;
} // namespace ncrequest
