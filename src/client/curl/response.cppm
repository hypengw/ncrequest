export module ncrequest:client_curl_response;
export import :request;
export import lihttpto;
export import :client_curl_connection;
export import :error;
export import ncrequest.coro;

using namespace rstd::prelude;
using rstd::bytes::Bytes;
using rstd::sync::Arc;

namespace ncrequest::client::curl
{

export class SessionBackend;

export class ResponseBackend {
    friend class SessionBackend;

public:
    ResponseBackend(const ResponseBackend&)                    = delete;
    auto operator=(const ResponseBackend&) -> ResponseBackend& = delete;

    class Inner;
    static constexpr usize ReadSize { 1024 * 16 };

public:
    auto header() const -> const lihttpto::Headers&;
    auto head() const -> Option<ref<lihttpto::MessageHead>>;
    auto trailers() const -> Option<ref<lihttpto::Headers>>;
    auto code() const -> Option<i32>;

    auto next_chunk() -> coro<Result<Option<Bytes>>>;
    auto ready_head() -> coro<Result<empty>> { co_return Ok(empty {}); }

    static auto make_response(PreparedRequest, SessionBackend&) -> Arc<ResponseBackend>;
    ResponseBackend(PreparedRequest, SessionBackend&) noexcept;
    ResponseBackend(ResponseBackend&&) noexcept;
    ~ResponseBackend() noexcept;
    ResponseBackend& operator=(ResponseBackend&&) noexcept;

    auto is_finished() const -> bool;
    auto request() const -> const Request&;

    auto pause_send(bool) -> bool;
    auto pause_recv(bool) -> bool;

    void cancel();

private:
    auto prepare_perform() -> Result<empty>;

    auto connection() -> Connection&;
    auto connection() const -> const Connection&;

private:
    Arc<Inner> m_inner;
};

class ResponseBackend::Inner {
public:
    Inner(ResponseBackend*, PreparedRequest, SessionBackend&);
    friend class ResponseBackend;

private:
    ResponseBackend* m_q;

    bool m_finished;

    Arc<Connection> m_connect;
};

} // namespace ncrequest::client::curl
