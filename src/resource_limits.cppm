export module ncrequest:resource_limits;
export import :error;
export import rstd;

using namespace rstd::prelude;

namespace ncrequest
{

export struct ResourceLimits {
    static constexpr usize DefaultHeaderBytes { 64 * 1024 };
    static constexpr usize DefaultReceiveBufferBytes { 64 * 1024 };
    static constexpr usize DefaultCollectBytes { 8 * 1024 * 1024 };

    // Per-hop transport header bytes, including informational heads and trailers.
    usize header_bytes         = DefaultHeaderBytes;
    usize receive_buffer_bytes = DefaultReceiveBufferBytes;
    // Collection only; streaming has no total-body cap. Zero permits an empty body.
    usize collect_bytes = DefaultCollectBytes;

    auto validate() const -> Result<empty> {
        if (header_bytes == usize())
            return Err(Error::InvalidState("header byte limit must be positive"));
        if (receive_buffer_bytes == usize())
            return Err(Error::InvalidState("receive buffer limit must be positive"));
        return Ok(empty {});
    }
};

} // namespace ncrequest
