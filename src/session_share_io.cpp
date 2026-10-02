module ncrequest;
import :session_share;

using namespace rstd::prelude;
using rstd::fs::read;
using rstd::fs::write;
using rstd::path::Path;

namespace ncrequest
{

auto SessionShare::load(ref<Path> path) -> Result<empty> {
    auto input = read(path);
    if (input.is_err()) return Err(Error::Io(rstd::move(input).unwrap_err()));
    for (auto byte : input->as_slice()) {
        if (byte == u8()) return Err(Error::InvalidState("cookie file contains NUL"));
    }
    return import_cookies(input->as_slice());
}

auto SessionShare::save(ref<Path> path) const -> Result<empty> {
    auto output = export_cookies();
    if (output.is_err()) return Err(rstd::move(output).unwrap_err());
    auto saved = write(path, output->as_slice());
    if (saved.is_err()) return Err(Error::Io(rstd::move(saved).unwrap_err()));
    return Ok(empty {});
}

} // namespace ncrequest
