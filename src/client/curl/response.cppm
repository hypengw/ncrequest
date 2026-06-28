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
    auto bytes() -> coro<Result<rstd::bytes::Bytes>>;

    template<typename SyncWriteStream>
        requires helper::is_sync_stream<SyncWriteStream>
    auto read_to_stream(SyncWriteStream& writer) -> coro<usize> {
        auto data_result = co_await bytes();
        if (data_result.is_err()) {
            rstd::panic { "ResponseBackend::read_to_stream failed" };
        }

        auto  data    = rstd::move(data_result).unwrap();
        usize written = 0;
        while (written < data.size()) {
            auto size = writer.write_some(slice<u8>::from_raw_parts(
                reinterpret_cast<const u8*>(data.data() + written), data.size() - written));
            if (size == 0) {
                rstd::panic { "ResponseBackend::read_to_stream made no progress" };
            }
            written += size;
        }
        co_return written;
    }

    static auto make_response(const Request&, Operation, Arc<SessionBackend>) -> Arc<ResponseBackend>;
    ResponseBackend(const Request&, Operation, Arc<SessionBackend>) noexcept;
    ResponseBackend(ResponseBackend&&) noexcept;
    ~ResponseBackend() noexcept;
    ResponseBackend& operator=(ResponseBackend&&) noexcept;

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
    void add_send_buffer(rstd::bytes::Bytes);

    auto connection() -> Connection&;
    auto connection() const -> const Connection&;

private:
    Arc<Inner> m_inner;
};

class ResponseBackend::Inner {
public:
    Inner(ResponseBackend*, const Request&, Operation, Arc<SessionBackend>);
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
