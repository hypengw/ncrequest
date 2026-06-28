export module ncrequest.coro:base;

export import rstd;

namespace ncrequest
{

export template<typename T, typename = void>
using coro = rstd::async::coro<T>;

} // namespace ncrequest
