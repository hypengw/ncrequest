export module ncrequest:client_curl_response;
export import :request;
export import :http;
export import :client_curl_connection;
export import :error;
export import ncrequest.coro;

namespace ncrequest::client::curl
{

export class SessionBackend;

export class ResponseBackend : public NoCopy {
    friend class SessionBackend;

public:
    using allocator_type = std::pmr::polymorphic_allocator<char>;
    class Inner;
    static constexpr usize ReadSize { 1024 * 16 };

public:
    auto header() const -> const lihttpto::Headers&;
    auto head() const -> rstd::Option<rstd::ref<lihttpto::MessageHead>>;
    auto trailers() const -> rstd::Option<rstd::ref<lihttpto::Headers>>;
    auto code() const -> rstd::Option<i32>;

    auto next_chunk() -> coro<Result<rstd::Option<rstd::bytes::Bytes>>>;
    auto ready_head() -> coro<Result<rstd::empty>> { co_return Ok(rstd::empty {}); }

    static auto make_response(const Request&, Operation, SessionBackend&) -> Arc<ResponseBackend>;
    ResponseBackend(const Request&, Operation, SessionBackend&) noexcept;
    ResponseBackend(ResponseBackend&&) noexcept;
    ~ResponseBackend() noexcept;
    ResponseBackend& operator=(ResponseBackend&&) noexcept;

    auto is_finished() const -> bool;
    auto request() const -> const Request&;
    auto operation() const -> Operation;

    auto pause_send(bool) -> bool;
    auto pause_recv(bool) -> bool;

    void cancel();
    auto allocator() const -> const allocator_type&;

private:
    void prepare_perform();
    void add_send_buffer(rstd::bytes::Bytes);

    auto connection() -> Connection&;
    auto connection() const -> const Connection&;

private:
    Arc<Inner> m_inner;
};

class ResponseBackend::Inner {
public:
    Inner(ResponseBackend*, const Request&, Operation, SessionBackend&);
    friend class ResponseBackend;

    void set_share(rstd::Option<SessionShare> share) { m_share = rstd::move(share); }

private:
    ResponseBackend* m_q;
    Request          m_req;

    Operation m_operation;
    bool      m_finished;

    rstd::bytes::Bytes         m_send_buffer;
    Arc<Connection>            m_connect;
    rstd::Option<SessionShare> m_share;

    std::pmr::polymorphic_allocator<char> m_allocator;
};

} // namespace ncrequest::client::curl
