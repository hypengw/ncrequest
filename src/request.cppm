module;
#include <rstd/enum.hpp>
#define REQ_OPT_PROP(Type, Name, Init)    \
    Type Name Init;                       \
    auto&     set_##Name(const Type& v) { \
        Name = v;                     \
        return *this;                 \
    }

export module ncrequest:request;
export import :http;
export import :session_share;
export import ncrequest.type;

namespace ncrequest
{

namespace req_opt
{
export struct Timeout {
    REQ_OPT_PROP(i64, low_speed, {})
    REQ_OPT_PROP(i64, connect_timeout, {})
    REQ_OPT_PROP(i64, transfer_timeout, {})
};

export struct Proxy : rstd::DefaultInClass<Proxy, rstd::clone::Clone> {
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
    auto clone() const -> Proxy { return *this; }
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

export struct Share : rstd::DefaultInClass<Share, rstd::clone::Clone> {
    rstd::Option<SessionShare> share {};
    auto&                      set_share(rstd::Option<SessionShare> v) {
        share = rstd::move(v);
        return *this;
    }
    // trait
    auto clone() const -> Share;
};

#undef REQ_OPT_PROP

} // namespace req_opt

export using RequestOpts =
    rstd::tuple<req_opt::Timeout, req_opt::Proxy, req_opt::Tcp, req_opt::SSL, req_opt::Read,
                req_opt::Share>;

export template<typename T>
concept RequestOption =
    rstd::mtp::same_as<rstd::mtp::decay<T>, req_opt::Timeout> ||
    rstd::mtp::same_as<rstd::mtp::decay<T>, req_opt::Proxy> ||
    rstd::mtp::same_as<rstd::mtp::decay<T>, req_opt::Tcp> ||
    rstd::mtp::same_as<rstd::mtp::decay<T>, req_opt::SSL> ||
    rstd::mtp::same_as<rstd::mtp::decay<T>, req_opt::Read> ||
    rstd::mtp::same_as<rstd::mtp::decay<T>, req_opt::Share>;

#define NCREQUEST_REQUEST_OPT_VARIANTS(V)  \
    V(Timeout, (req_opt::Timeout value;))  \
    V(Proxy, (req_opt::Proxy value;))      \
    V(Tcp, (req_opt::Tcp value;))          \
    V(SSL, (req_opt::SSL value;))          \
    V(Read, (req_opt::Read value;))        \
    V(Share, (req_opt::Share value;))

export struct RequestOpt {
    RSTD_ENUM_BODY(RequestOpt, NCREQUEST_REQUEST_OPT_VARIANTS)
};

#undef NCREQUEST_REQUEST_OPT_VARIANTS

export auto global_init(std::pmr::memory_resource* resource = nullptr) -> std::error_code;
} // namespace ncrequest
namespace ncrequest
{

export class Request : public rstd::DefaultInClass<Request, rstd::clone::Clone> {
public:
    Request() noexcept;
    Request(std::string_view url) noexcept;
    Request(Request&&) noexcept;
    ~Request() noexcept;
    Request& operator=(Request&&) noexcept;

    auto url() const -> std::string_view;
    auto url_info() const -> const URI&;
    auto set_url(std::string_view) -> Request&;

    auto header() const -> const Header&;
    auto header(std::string_view name) const -> std::string;
    auto update_header(const Header&) -> Request&;
    auto set_header(std::string_view name, std::string_view value) -> Request&;
    auto remove_header(std::string_view name) -> Request&;
    void set_opt(const Header&);

    template<RequestOption T>
    T& get_opt() {
        return m_opts.template get<T>();
    }

    template<RequestOption T>
    const T& get_opt() const {
        return m_opts.template get<T>();
    }

    void set_opt(RequestOpt&&);

    template<RequestOption T>
    auto set_opt(T&& opt) -> Request& {
        m_opts.template get<rstd::mtp::decay<T>>() = rstd::forward<T>(opt);
        return *this;
    }

    // trait
    auto clone() const -> ncrequest::Request;

private:
    URI         m_uri;
    Header      m_header;
    RequestOpts m_opts;
};

} // namespace ncrequest

export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::Request>
    : rstd::LinkClassMethod<rstd::clone::Clone, ncrequest::Request> {};
export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::req_opt::Share>
    : rstd::LinkClassMethod<rstd::clone::Clone, ncrequest::req_opt::Share> {};
export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::req_opt::Proxy>
    : rstd::LinkClassMethod<rstd::clone::Clone, ncrequest::req_opt::Proxy> {};
