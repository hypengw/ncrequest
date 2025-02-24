module;
#define REQ_OPT_PROP(Type, Name, Init)    \
    Type Name Init;                       \
    auto&     set_##Name(const Type& v) { \
        Name = v;                     \
        return *this;                 \
    }

#define C_DECLARE_PUBLIC(Class, QName)                                              \
    inline Class*       q_func() { return static_cast<Class*>(QName); }             \
    inline const Class* q_func() const { return static_cast<const Class*>(QName); } \
    friend class Class;

#include <string_view>
#include <functional>
#include <optional>
#include <variant>
#include <system_error>

export module ncrequest:request;
export import :http;
export import :session_share;
export import ncrequest.type;
export import ncrequest.type_list;

namespace ncrequest
{
namespace req_opt
{
export struct Timeout {
    REQ_OPT_PROP(i64, low_speed, {})
    REQ_OPT_PROP(i64, connect_timeout, {})
    REQ_OPT_PROP(i64, transfer_timeout, {})
};

export struct Proxy {
    enum class Type
    {
        HTTP    = 0,
        HTTPS2  = 3,
        SOCKS4  = 4,
        SOCKS5  = 5,
        SOCKS4A = 6,
        SOCKS5H = 7
    };
    REQ_OPT_PROP(Type, type, { Type::HTTP })
    REQ_OPT_PROP(std::string, content, {})
};

export struct Tcp {
    REQ_OPT_PROP(bool, keepalive, {})
    REQ_OPT_PROP(i64, keepidle, {})
    REQ_OPT_PROP(i64, keepintvl, {})
};

export struct SSL {
    REQ_OPT_PROP(bool, verify_certificate, { true })
};

export struct Read {
    using Callback = std::function<usize(byte* ptr, usize size)>;
    REQ_OPT_PROP(Callback, callback, {})
    REQ_OPT_PROP(usize, size, { 0 })
};

export struct Share {
    REQ_OPT_PROP(std::optional<SessionShare>, share, {})
};

#undef REQ_OPT_PROP

using opts = type_list<Timeout, Proxy, Tcp, SSL, Read, Share>;

} // namespace req_opt

export using RequestOpts = req_opt::opts;
export using RequestOpt  = RequestOpts::to<std::variant>;

export class Session;
export class Response;

export auto global_init() -> std::error_code;

export class Request {
    friend class Session;
    friend class Response;

public:
    class Private;
    Request() noexcept;
    Request(std::string_view url) noexcept;
    ~Request() noexcept;

    auto url() const -> std::string_view;
    auto url_info() const -> const URI&;
    auto set_url(std::string_view) -> Request&;

    auto header() const -> const Header&;
    auto header(std::string_view name) const -> std::string;
    auto update_header(const Header&) -> Request&;
    auto set_header(std::string_view name, std::string_view value) -> Request&;
    auto remove_header(std::string_view name) -> Request&;
    void set_opt(const Header&);

    template<typename T>
    T& get_opt() {
        constexpr auto idx = RequestOpts::index<T>();
        return *(static_cast<T*>(get_opt(idx)));
    }

    template<typename T>
    const T& get_opt() const {
        constexpr auto idx = RequestOpts::index<T>();
        return *(static_cast<const T*>(get_opt(idx)));
    }

    void set_opt(const RequestOpt&);

private:
    const_voidp get_opt(usize) const;
    voidp       get_opt(usize);

    URI                         m_uri;
    Header                      m_header;
    RequestOpts::to<std::tuple> m_opts;
};

} // namespace ncrequest
