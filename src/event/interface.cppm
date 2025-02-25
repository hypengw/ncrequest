module;

#include <functional>
#include <string_view>

export module ncrequest.event:interface;

namespace ncrequest::event
{

export enum class WaitType { Read, Write };

export class Context {
public:
    using WaitType      = event::WaitType;
    using ErrorCallback = std::function<void(std::string_view)>;
    using EventCallback = std::function<void()>;

    virtual ~Context() = default;

    virtual bool assign(int socket_fd) = 0;
    virtual void close()               = 0;

    virtual void wait(WaitType type, EventCallback callback) = 0;
    virtual void cancel()                                    = 0;

    virtual void set_error_callback(ErrorCallback callback) = 0;

    virtual void post(std::function<void()>) = 0;
};

} // namespace ncrequest::event
