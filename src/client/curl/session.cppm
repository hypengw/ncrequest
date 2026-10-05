export module ncrequest:client.curl.session;
export import :request;
export import :client.curl.response;
export import :client.curl.connection;
export import rstd;

using namespace rstd::prelude;
using rstd::bytes::Bytes;
using rstd::path::Path;
using rstd::sync::Arc;

namespace ncrequest::client::curl
{

export class SessionBackend {
public:
    SessionBackend(const SessionBackend&)                    = delete;
    auto operator=(const SessionBackend&) -> SessionBackend& = delete;

    using channel_type = SessionChannel;

    class Private;
    ~SessionBackend();

    explicit SessionBackend(CurlOptions options = {});

    template<typename... Args>
    static auto make(Args&&... args) -> Result<Arc<SessionBackend>> {
        auto initialized = initialize();
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto session = Arc<SessionBackend>::make(rstd::forward<Args>(args)...);
        auto started = session->start();
        if (started.is_err()) return Err(rstd::move(started).unwrap_err());
        return Ok(rstd::move(session));
    }

    static auto initialize() -> Result<empty> { return global_init(); }
    auto        start() -> Result<empty>;

    auto start_request(PreparedRequest) -> coro<Result<ResponseBackend>>;

    void about_to_stop();
    void close() { about_to_stop(); }

    auto channel() -> channel_type&;
    auto channel_rc() -> Arc<channel_type>;

private:
    auto perform(Arc<ResponseBackend>&) -> coro<Result<empty>>;

    Box<Private> m_d;
};

export using Options = CurlOptions;

} // namespace ncrequest::client::curl
