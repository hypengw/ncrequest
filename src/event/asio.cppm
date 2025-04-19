module;

#include <asio/post.hpp>

export module ncrequest.event:asio;
export import ncrequest.type;
import :interface;

namespace ncrequest::event
{

template<typename Socket>
class AsioContext : public ncrequest::event::Context {
public:
    using executor_type = typename Socket::executor_type;
    using wait_type     = typename Socket::wait_type;

    explicit AsioContext(executor_type ioc): m_socket(std::move(ioc)) {}

    bool assign(int socket_fd) override {
        asio::error_code code;
        m_socket.assign(socket_fd, code);

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
                            [callback = std::move(callback), this](const asio::error_code& ec) {
                                if (! ec) {
                                    callback();
                                } else if (m_on_error) {
                                    m_on_error(ec.message().c_str());
                                }
                            });
    }

    void cancel() override { m_socket.cancel(); }

    void set_error_callback(ErrorCallback callback) override { m_on_error = std::move(callback); }

    void post(std::function<void()> f) override { asio::post(m_socket.get_executor(), f); }

private:
    Socket        m_socket;
    ErrorCallback m_on_error;
};

export template<template<typename> class Socket, typename Ex>
auto create(Ex&& ex) -> Box<Context> {
    return ncrequest::make_box<AsioContext<Socket<std::decay_t<Ex>>>>(std::forward<Ex>(ex));
}

} // namespace ncrequest::event
