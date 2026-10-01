export module ncrequest:client_curl_session;
export import :request;
export import :client_curl_response;
export import :client_curl_connection;
export import ncrequest.type;

using rstd::bytes::Bytes;
using rstd::path::Path;
using rstd::string::String;
using rstd::vec::Vec;
using std::pmr::memory_resource;
using std::pmr::polymorphic_allocator;

namespace ncrequest::client::curl
{

export class SessionBackend : public NoCopy {
public:
    using channel_type = SessionChannel;

    class Private;
    ~SessionBackend();

    explicit SessionBackend(memory_resource*    = std::pmr::get_default_resource(),
                            CurlOptions options = {});

    template<typename... Args>
    static auto make(Args&&... args) -> Arc<SessionBackend> {
        auto session = Arc<SessionBackend>::make(rstd::forward<Args>(args)...);
        session->start();
        return session;
    }

    void start();

    auto start_request(const Request&, Operation, rstd::Option<Bytes>)
        -> coro<Result<ResponseBackend>>;

    auto get(const Request&) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request&) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request&, Bytes) -> coro<Result<Arc<ResponseBackend>>>;

    auto cookies() -> Vec<String>;
    void load_cookie(ref<Path> path);
    void save_cookie(ref<Path> path) const;
    void set_proxy(const req_opt::Proxy&);
    void set_verify_certificate(bool);

    void about_to_stop();
    void close() { about_to_stop(); }

    auto channel() -> channel_type&;
    auto channel_rc() -> Arc<channel_type>;
    auto allocator() -> polymorphic_allocator<byte>;

private:
    auto perform(Arc<ResponseBackend>&) -> coro<Result<rstd::empty>>;
    auto prepare_req(const Request&) const -> Request;

    Box<Private> m_d;
};

export using Options = CurlOptions;

} // namespace ncrequest::client::curl
