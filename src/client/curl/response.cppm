export module ncrequest:client_curl_response;
export import :request;
export import :http;
export import :client_curl_connection;
export import :error;
export import ncrequest.coro;

using rstd::bytes::Bytes;
using std::pmr::polymorphic_allocator;

namespace ncrequest::client::curl
{

export class SessionBackend;

export class ResponseBackend : public NoCopy {
    friend class SessionBackend;

public:
    using allocator_type = polymorphic_allocator<char>;
    class Inner;
    static constexpr usize ReadSize { 1024 * 16 };

public:
    auto header() const -> const lihttpto::Headers&;
    auto head() const -> rstd::Option<rstd::ref<lihttpto::MessageHead>>;
    auto trailers() const -> rstd::Option<rstd::ref<lihttpto::Headers>>;
    auto code() const -> rstd::Option<i32>;

    auto next_chunk() -> coro<Result<rstd::Option<Bytes>>>;
    auto ready_head() -> coro<Result<rstd::empty>> { co_return Ok(rstd::empty {}); }

    static auto make_response(Request, SessionBackend&) -> Arc<ResponseBackend>;
    ResponseBackend(Request, SessionBackend&) noexcept;
    ResponseBackend(ResponseBackend&&) noexcept;
    ~ResponseBackend() noexcept;
    ResponseBackend& operator=(ResponseBackend&&) noexcept;

    auto is_finished() const -> bool;
    auto request() const -> const Request&;

    auto pause_send(bool) -> bool;
    auto pause_recv(bool) -> bool;

    void cancel();
    auto allocator() const -> const allocator_type&;

private:
    auto prepare_perform() -> Result<rstd::empty>;

    auto connection() -> Connection&;
    auto connection() const -> const Connection&;

private:
    Arc<Inner> m_inner;
};

class ResponseBackend::Inner {
public:
    Inner(ResponseBackend*, Request, SessionBackend&);
    friend class ResponseBackend;

private:
    ResponseBackend* m_q;

    bool m_finished;

    Arc<Connection> m_connect;

    polymorphic_allocator<char> m_allocator;
};

} // namespace ncrequest::client::curl
