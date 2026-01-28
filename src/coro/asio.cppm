module;

#include <asio/awaitable.hpp>
#include <asio/dispatch.hpp>
#include <asio/co_spawn.hpp>

#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <asio/any_completion_handler.hpp>

#include <asio/buffer.hpp>
#include <asio/buffers_iterator.hpp>
#include <asio/streambuf.hpp>

#include <asio/associated_executor.hpp>
#include <asio/bind_executor.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/as_tuple.hpp>
#include <asio/deferred.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include <asio/read.hpp>
#include <asio/experimental/concurrent_channel.hpp>

export module ncrequest.coro:asio;

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

constexpr detached_t detached_;

namespace detail
{
using asio::detail::throw_error;
using asio::detail::default_max_transfer_size;
}

namespace stream_errc
{
using asio::stream_errc::eof;
}

namespace error
{
using asio::error::make_error_code;
using asio::error::misc_errors;
using asio::error::basic_errors;
}

namespace experimental
{
using asio::experimental::concurrent_channel;
}

} // namespace asio