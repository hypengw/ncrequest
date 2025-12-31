module;
#include <unistd.h>
#include <sys/socket.h>

export module ncrequest.event:asio;
export import ncrequest.type;
export import ncrequest.coro;
import :interface;

namespace ncrequest::event
{

template<typename Socket>
class AsioContext : public ncrequest::event::Context {
public:
    using executor_type = typename Socket::executor_type;
    using wait_type     = typename Socket::wait_type;

    explicit AsioContext(executor_type ioc): m_socket(rstd::move(ioc)) {}

    bool assign(socket_t socket_fd) override {
        asio::error_code code;

        if constexpr (requires() { typename Socket::protocol_type; }) {
            int       type   = 0;
            socklen_t optlen = sizeof(type);
            if (auto rc = getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, (char*)&type, &optlen);
                rc != 0) {
                m_on_error(rstd::format("getsocketopt failed: {}", rc));
                return false;
            }

            sockaddr_storage addr;
            socklen_t        addrlen = sizeof(addr);
            if (auto rc = getsockname(socket_fd, (sockaddr*)&addr, &addrlen); rc != 0) {
                m_on_error(rstd::format("getsocketopt failed: {}", rc));
                return false;
            }
            m_socket.assign(typename Socket::protocol_type(addr.ss_family, type), socket_fd, code);
        } else {
            m_socket.assign(socket_fd, code);
        }

        if (code) {
            m_on_error(code.message());
            return false;
        }
        return true;
    }

    void close() override { m_socket.close(); }
    void reset() override { m_socket.release(); }

    void wait(WaitType type, EventCallback callback) override {
        auto wait_type = wait_type::wait_read;
        switch (type) {
        case WaitType::Read: wait_type = wait_type::wait_read; break;
        case WaitType::Write: wait_type = wait_type::wait_write; break;
        }

        m_socket.async_wait(wait_type,
                            [callback = rstd::move(callback), this](const asio::error_code& ec) {
                                if (! ec) {
                                    callback();
                                } else if (m_on_error) {
                                    m_on_error(ec.message().c_str());
                                }
                            });
    }

    void cancel() override { m_socket.cancel(); }

    void set_error_callback(ErrorCallback callback) override { m_on_error = rstd::move(callback); }

    void post(rstd::cppstd::function<void()> f) override { asio::post(m_socket.get_executor(), f); }

private:
    Socket        m_socket;
    ErrorCallback m_on_error;
};

export template<template<typename> class Socket, typename Ex>
auto create(Ex&& ex) -> Box<Context> {
    return ncrequest::make_box<AsioContext<Socket<rstd::meta::decay_t<Ex>>>>(rstd::forward<Ex>(ex));
}

} // namespace ncrequest::event
