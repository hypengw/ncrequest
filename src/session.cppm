module;

#include <optional>
#include <filesystem>
#include <memory_resource>

#include <asio/thread_pool.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/strand.hpp>

export module ncrequest:session;
export import :request;
export import :response;
export import :connection;
export import ncrequest.type;

namespace ncrequest
{

export class Session : public std::enable_shared_from_this<Session>, NoCopy {
    friend class Request;
    friend class Response;

public:
    using executor_type = asio::thread_pool::executor_type;
    using channel_type  = SessionChannel;

    class Private;
    ~Session();

    static auto make(executor_type ex, std::pmr::memory_resource* = std::pmr::get_default_resource()) -> Arc<Session>;

    auto get_executor() -> executor_type&;
    auto get_strand() -> asio::strand<executor_type>&;
    auto get_arc() { return shared_from_this(); }

    auto get(const Request&) -> coro<rstd::Option<Arc<Response>>>;
    auto post(const Request&) -> coro<rstd::Option<Arc<Response>>>;
    auto post(const Request&, asio::const_buffer) -> coro<rstd::Option<Arc<Response>>>;

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
    Session(executor_type ex, std::pmr::memory_resource* = std::pmr::get_default_resource());
    auto perform(Arc<Response>&) -> coro<bool>;
    auto prepare_req(const Request&) const -> Request;

    Box<Private>          m_d;
    inline Private*       d_func() { return m_d.get(); }
    inline const Private* d_func() const { return m_d.get(); }
};

} // namespace ncrequest
