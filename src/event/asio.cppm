module;

#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>

export module ncrequest.event:asio;
export import ncrequest.type;
import :interface;

namespace ncrequest::event
{

export auto create(asio::io_context& ioc) -> box<Context>;
export auto create(asio::thread_pool& ioc) -> box<Context>;

} // namespace ncrequest::event
