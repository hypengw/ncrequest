#pragma once

#include <optional>
#include <filesystem>
#include <memory_resource>

#include <asio/thread_pool.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/strand.hpp>
#include <asio/experimental/concurrent_channel.hpp>

#include "ncrequest/type.hpp"
#include "ncrequest/request_opt.hpp"

namespace ncrequest
{

class Request;
class Response;
class Session : public std::enable_shared_from_this<Session>, NoCopy {
    friend class Request;
    friend class Response;

public:
    using executor_type = asio::thread_pool::executor_type;
    using channel_type =
        asio::experimental::concurrent_channel<asio::strand<executor_type>,
                                               void(asio::error_code, SessionMessage)>;

    class Private;
    Session(executor_type ex);
    ~Session();

    auto get_executor() -> executor_type&;
    auto get_strand() -> asio::strand<executor_type>&;
    auto get_rc() { return shared_from_this(); }

    auto get(const Request&) -> asio::awaitable<std::optional<rc<Response>>>;
    auto post(const Request&) -> asio::awaitable<std::optional<rc<Response>>>;
    auto post(const Request&, asio::const_buffer) -> asio::awaitable<std::optional<rc<Response>>>;

    auto cookies() -> std::vector<std::string>;
    void load_cookie(std::filesystem::path);
    void save_cookie(std::filesystem::path) const;
    void set_proxy(const req_opt::Proxy&);
    void set_verify_certificate(bool);

    void about_to_stop();

    auto channel() -> channel_type&;
    auto channel_rc() -> rc<channel_type>;
    auto allocator() -> std::pmr::polymorphic_allocator<byte>;

private:
    auto perform(rc<Response>&) -> asio::awaitable<bool>;
    auto prepare_req(const Request&) const -> Request;

    up<Private>           m_d;
    inline Private*       d_func() { return m_d.get(); }
    inline const Private* d_func() const { return m_d.get(); }
};

} // namespace ncrequest
