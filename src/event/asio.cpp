module;

#include <asio/post.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>
#include <asio/posix/stream_descriptor.hpp>

module ncrequest.event;
import :asio;

namespace
{

template<typename Executor>
class AsioContext : public ncrequest::event::Context {
public:
    explicit AsioContext(Executor ioc): m_socket(std::move(ioc)) {}

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
                                    printf("error %d\n", ec.value());
                                    m_on_error(ec.message().c_str());
                                }
                            });
    }

    void cancel() override { m_socket.cancel(); }

    void set_error_callback(ErrorCallback callback) override { m_on_error = std::move(callback); }

    void post(std::function<void()> f) override { asio::post(m_socket.get_executor(), f); }

private:
    asio::posix::basic_stream_descriptor<Executor> m_socket;
    ErrorCallback                                  m_on_error;
};
} // namespace

namespace ncrequest
{
auto event::create(asio::io_context& ioc) -> box<Context> {
    return make_box<AsioContext<asio::io_context::executor_type>>(ioc.get_executor());
}
auto event::create(asio::thread_pool& ioc) -> box<Context> {
    return make_box<AsioContext<asio::thread_pool::executor_type>>(ioc.get_executor());
}
} // namespace ncrequest