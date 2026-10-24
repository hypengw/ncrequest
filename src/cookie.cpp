module ncrequest;
import :cookie;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest
{
auto cookie_io(rstd::io::Error cause) -> Error {
    return Error::Transport(lifetchto::Error::Extension(
        "cookie file"_Str, Arc<rstd::dyn<rstd::error::Error>>::make(rstd::move(cause))));
}
auto load_cookies(const CookieJar& jar, ref<rstd::path::Path> path) -> Result<empty> {
    auto bytes = rstd::fs::read(path);
    if (bytes.is_err()) return Err(cookie_io(rstd::move(bytes).unwrap_err()));
    auto text = rstd::str_::from_utf8(bytes->as_slice());
    if (text.is_err()) return Err(Error::decode(text.unwrap_err()));
    auto loaded = jar.import_netscape(*text, rstd::time::SystemTime::now().as_unix_time().seconds);
    if (loaded.is_err()) return Err(Error::Transport(rstd::move(loaded).unwrap_err()));
    return Ok(empty {});
}
auto save_cookies(const CookieJar& jar, ref<rstd::path::Path> path) -> Result<empty> {
    auto bytes = jar.export_netscape(rstd::time::SystemTime::now().as_unix_time().seconds);
    if (bytes.is_err()) return Err(Error::Transport(rstd::move(bytes).unwrap_err()));
    auto written = rstd::fs::write(path, bytes->as_str().as_bytes());
    if (written.is_err()) return Err(cookie_io(rstd::move(written).unwrap_err()));
    return Ok(empty {});
}
} // namespace ncrequest
