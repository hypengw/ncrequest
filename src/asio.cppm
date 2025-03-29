module;
#include <asio/buffer.hpp>
#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

export module ncrequest.coro;

namespace ncrequest
{

export template<typename T, typename Ex = asio::any_io_executor>
using coro = asio::awaitable<T, Ex>;
export constexpr asio::use_awaitable_t<> use_coro;

export using const_buffer   = asio::const_buffer;
export using mutable_buffer = asio::mutable_buffer;
export using CoroError      = asio::error_code;

} // namespace ncrequest