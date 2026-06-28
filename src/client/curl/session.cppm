export module ncrequest:client_curl_session;
export import :request;
export import :client_curl_response;
export import :client_curl_connection;
export import ncrequest.type;

namespace ncrequest::client::curl
{

export class SessionBackend : public std::enable_shared_from_this<SessionBackend>, NoCopy {
public:
    using channel_type = SessionChannel;

    class Private;
    ~SessionBackend();

    explicit SessionBackend(std::pmr::memory_resource* = std::pmr::get_default_resource(),
                            CurlOptions options = {});

    template<typename... Args>
    static auto make(Args&&... args) -> Arc<SessionBackend> {
        auto session = make_arc<SessionBackend>(rstd::forward<Args>(args)...);
        session->start();
        return session;
    }

    void start();

    auto get_arc() { return shared_from_this(); }

    auto start_request(const Request&, Operation, rstd::Option<rstd::bytes::Bytes>)
        -> coro<Result<ResponseBackend>>;

    auto get(const Request&) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request&) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request&, rstd::bytes::Bytes) -> coro<Result<Arc<ResponseBackend>>>;

    auto cookies() -> std::vector<std::string>;
    void load_cookie(std::filesystem::path);
    void save_cookie(std::filesystem::path) const;
    void set_proxy(const req_opt::Proxy&);
    void set_verify_certificate(bool);

    void about_to_stop();

    auto channel() -> channel_type&;
    auto channel_rc() -> Arc<channel_type>;
    auto allocator() -> std::pmr::polymorphic_allocator<byte>;

private:
    auto perform(Arc<ResponseBackend>&) -> coro<Result<rstd::empty>>;
    auto prepare_req(const Request&) const -> Request;

    Box<Private>          m_d;
    inline Private*       d_func() { return m_d.get(); }
    inline const Private* d_func() const { return m_d.get(); }
};

export using Options = CurlOptions;

} // namespace ncrequest::client::curl
