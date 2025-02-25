module;

#include <asio.hpp>

export module ncrequest.event:asio;
export import ncrequest.type;
import :interface;

namespace ncrequest::event
{
class AsioContext : public Context {
public:
    explicit AsioContext(asio::io_context& ioc): m_socket(ioc) {}

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

    void wait(WaitType type, EventCallback callback) override {
        auto wait_type = asio::posix::stream_descriptor::wait_read;
        switch (type) {
        case WaitType::Read: wait_type = asio::posix::stream_descriptor::wait_read; break;
        case WaitType::Write: wait_type = asio::posix::stream_descriptor::wait_write; break;
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

private:
    asio::posix::stream_descriptor m_socket;
    ErrorCallback                  m_on_error;
};

export auto create(asio::io_context& ioc) -> box<Context> { return make_box<AsioContext>(ioc); }

} // namespace ncrequest::event
