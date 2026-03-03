module;

#include <asio.hpp>

#include <asio/experimental/concurrent_channel.hpp>
#include <asio/experimental/parallel_group.hpp>

export module asio;

export namespace asio
{
using asio::any_io_executor;
using asio::awaitable;
using asio::use_awaitable_t;

using asio::any_completion_handler;
using asio::as_tuple;
using asio::async_initiate;
using asio::async_read;
using asio::basic_streambuf;
using asio::bind_executor;
using asio::buffer;
using asio::buffer_copy;
using asio::buffers_begin;
using asio::buffers_end;
using asio::co_spawn;
using asio::const_buffer;
using asio::dispatch;
using asio::error_code;
using asio::is_const_buffer_sequence;
using asio::make_strand;
using asio::mutable_buffer;
using asio::post;
using asio::strand;
using asio::streambuf;
using asio::thread_pool;
using asio::transfer_all;

using asio::bind_allocator;

using asio::deferred_t;
using asio::execution_context;
using asio::get_associated_executor;

using asio::system_error;

using asio::steady_timer;

using asio::is_const_buffer_sequence;
using asio::is_mutable_buffer_sequence;
using asio::recycling_allocator;

using asio::use_awaitable_t;
using asio::use_future_t;

constexpr detached_t detached_;

namespace detail
{
using asio::detail::default_max_transfer_size;
using asio::detail::throw_error;
} // namespace detail

namespace stream_errc
{
using asio::stream_errc::eof;
}

namespace error
{
using asio::error::basic_errors;
using asio::error::get_system_category;
using asio::error::make_error_code;
using asio::error::misc_errors;
} // namespace error

namespace execution
{
using asio::execution::blocking_t;
using asio::execution::context_t;
}; // namespace execution

namespace this_coro
{
using asio::this_coro::executor_t;
constexpr auto executor_ { executor };
} // namespace this_coro

namespace chrono
{
using asio::chrono::microseconds;
using asio::chrono::milliseconds;
using asio::chrono::minutes;
using asio::chrono::seconds;
} // namespace chrono

namespace experimental
{
using asio::experimental::concurrent_channel;
using asio::experimental::make_parallel_group;
using asio::experimental::wait_for_one;
} // namespace experimental

#ifdef _WIN32
using asio::basic_stream_socket;
#else
namespace posix
{
using asio::posix::basic_stream_descriptor;
}
#endif

} // namespace asio