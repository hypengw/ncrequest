module;

#include <limits>
#include <optional>

#include <asio/any_completion_handler.hpp>
#include <asio/read.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>
#include <asio/streambuf.hpp>
#include <asio/bind_executor.hpp>
#include <asio/as_tuple.hpp>

export module ncrequest:response;
export import :request;
export import :http;
export import :connection;
export import :error;

namespace ncrequest
{

class Session;

class Response : public NoCopy {
    friend class Session;

public:
    using executor_type  = asio::strand<asio::thread_pool::executor_type>;
    using allocator_type = std::pmr::polymorphic_allocator<char>;
    class Inner;
    static constexpr usize ReadSize { 1024 * 16 };

public:
    executor_type& get_executor();

    template<Attribute A, typename T = attr_type<A>>
    auto attribute(void) const -> rstd::Option<T> {
        auto a = attribute(A);
        if (a.index() == 0) {
            return std::nullopt;
        }
        return std::get<T>(a);
    }

    auto attribute(Attribute) const -> attr_value;

    auto header() const -> const HttpHeader&;
    auto code() const -> rstd::Option<i32>;

    auto text() -> coro<Result<std::string>>;
    auto bytes() -> coro<Result<std::vector<byte>>>;

    template<typename MB, typename CompletionToken>
        requires asio::is_const_buffer_sequence<MB>::value
    auto async_read_some(const MB& buffer, CompletionToken&& token) {
        using ret = void(asio::error_code, std::size_t);
        return asio::async_initiate<CompletionToken, ret>(
            [&](auto&& handler) {
                asio::mutable_buffer mu_buf { asio::buffer(buffer) };
                async_read_some_impl(mu_buf, std::move(handler));
            },
            token);
    }

    template<typename MB, typename CompletionToken>
        requires asio::is_const_buffer_sequence<MB>::value
    auto async_write_some(const MB& buffer, CompletionToken&& token) {
        using ret = void(asio::error_code, std::size_t);
        return asio::async_initiate<CompletionToken, ret>(
            [&](auto&& handler) {
                auto const_buf = asio::const_buffer(buffer);
                async_read_some_impl(const_buf, std::move(handler));
            },
            token);
    }

    template<typename SyncWriteStream>
        requires helper::is_sync_stream<SyncWriteStream>
    auto read_to_stream(SyncWriteStream& writer) -> coro<usize> {
        asio::basic_streambuf<allocator_type> buf(std::numeric_limits<usize>::max(), allocator());
        buf.prepare(ReadSize);

        auto [ec, size] = co_await asio::async_read(
            *this,
            buf,
            [&buf, &writer](const auto& err, std::size_t) -> std::size_t {
                buf.consume(writer.write_some(buf.data()));
                return ! ! err ? 0 : asio::detail::default_max_transfer_size;
            },
            asio::as_tuple(asio::bind_executor(get_executor(), use_coro)));
        if (ec != asio::stream_errc::eof) {
            asio::detail::throw_error(ec);
        }
        co_return size;
    }

    static auto make_response(const Request&, Operation, Arc<Session>) -> Arc<Response>;
    Response(const Request&, Operation, Arc<Session>) noexcept;
    Response(Response&&) noexcept;
    ~Response() noexcept;
    Response& operator=(Response&&) noexcept;

    auto is_finished() const -> bool;
    auto request() const -> const Request&;
    auto operation() const -> Operation;

    auto cookie_jar() const -> const CookieJar&;

    auto pause_send(bool) -> bool;
    auto pause_recv(bool) -> bool;

    void cancel();
    auto allocator() const -> const allocator_type&;

private:
    void prepare_perform();
    void add_send_buffer(asio::const_buffer);
    void async_read_some_impl(asio::mutable_buffer,
                              asio::any_completion_handler<void(asio::error_code, usize)>);

    void async_write_some_impl(asio::const_buffer,
                               asio::any_completion_handler<void(asio::error_code, usize)>);

    void done(int);
    auto connection() -> Connection&;
    auto connection() const -> const Connection&;

private:
    Arc<Inner> m_inner;
};

class Response::Inner {
public:
    Inner(Response*, const Request&, Operation, Arc<Session>);
    friend class Response;

    void set_share(rstd::Option<SessionShare> share) { m_share = std::move(share); }

private:
    Response* m_q;
    Request   m_req;

    Operation m_operation;
    bool      m_finished;

    asio::streambuf            m_send_buffer;
    Arc<Connection>            m_connect;
    rstd::Option<SessionShare> m_share;

    std::pmr::polymorphic_allocator<char> m_allocator;
};

} // namespace ncrequest
