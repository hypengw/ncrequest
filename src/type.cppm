export module ncrequest.type;
export import ncrequest.coro;
export import rstd;

namespace ncrequest
{

export using namespace rstd::prelude;

export using rstd::boxed::Box;
export using rstd::sync::Arc;
export using rstd::sync::Weak;

export struct NoCopy {
protected:
    NoCopy()  = default;
    ~NoCopy() = default;

    NoCopy(NoCopy&&)            = default;
    NoCopy& operator=(NoCopy&&) = default;

    NoCopy(const NoCopy&)            = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};

namespace helper
{

export template<typename T>
concept is_sync_stream = requires(T s, slice<u8> buf) {
    { s.write_some(buf) } -> rstd::mtp::convertible_to<usize>;
};

} // namespace helper
} // namespace ncrequest
