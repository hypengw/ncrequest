module;

#include <functional>
#include <string_view>

export module ncrequest.event:interface;

namespace ncrequest::event
{

export enum class WaitType { Read, Write };

/* socket typedef */
#if defined(_WIN32) && ! defined(__LWIP_OPT_H__) && ! defined(LWIP_HDR_OPT_H)
using socket_t            = SOCKET;
constexpr auto SOCKET_BAD = INVALID_SOCKET;
#else
using socket_t            = int;
constexpr auto SOCKET_BAD = -1;
#endif

export class Context {
public:
    using WaitType      = event::WaitType;
    using ErrorCallback = std::function<void(std::string_view)>;
    using EventCallback = std::function<void()>;

    virtual ~Context() = default;

    virtual bool assign(socket_t socket_fd) = 0;
    virtual void close()                    = 0;
    virtual void reset()                    = 0;

    virtual void wait(WaitType type, EventCallback callback) = 0;
    virtual void cancel()                                    = 0;

    virtual void set_error_callback(ErrorCallback callback) = 0;

    virtual void post(std::function<void()>) = 0;
};

} // namespace ncrequest::event
