export module ncrequest.coro;
export import :asio;

namespace ncrequest
{

export template<typename T, typename Ex = asio::any_io_executor>
using coro = asio::awaitable<T, Ex>;
export constexpr asio::use_awaitable_t<> use_coro; 

export using CoroError = asio::error_code;

} // namespace ncrequest