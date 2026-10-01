module;
#include <rstd/enum.hpp>
#define REQ_OPT_PROP(Type, Name, Init)    \
    Type Name Init;                       \
    auto&     set_##Name(const Type& v) { \
        Name = v;                         \
        return *this;                     \
    }

export module ncrequest:request;
export import :http;
export import :error;
export import :session_share;
export import ncrequest.type;

using rstd::clone::Clone;
using rstd::mtp::decay;
using rstd::mtp::same_as;
using std::pmr::memory_resource;

namespace ncrequest
{

export struct Operation {
    RSTD_ENUM(Operation, (Get), (Post), (Delete), (Head))
};

namespace req_opt
{
export struct Timeout {
    REQ_OPT_PROP(i64, low_speed, {})
    REQ_OPT_PROP(i64, connect_timeout, {})
    REQ_OPT_PROP(i64, transfer_timeout, {})
};

export struct Proxy : rstd::DefaultInClass<Proxy, Clone> {
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

export struct Share : rstd::DefaultInClass<Share, Clone> {
    rstd::Option<SessionShare> share {};
    auto&                      set_share(rstd::Option<SessionShare> v) {
        share = rstd::move(v);
        return *this;
    }
    auto clone() const -> Share;
};

#undef REQ_OPT_PROP

} // namespace req_opt

export using RequestOpts = rstd::tuple<req_opt::Timeout, req_opt::Proxy, req_opt::Tcp, req_opt::SSL,
                                       req_opt::Read, req_opt::Share>;

export template<typename T>
concept RequestOption = same_as<decay<T>, req_opt::Timeout> || same_as<decay<T>, req_opt::Proxy> ||
                        same_as<decay<T>, req_opt::Tcp> || same_as<decay<T>, req_opt::SSL> ||
                        same_as<decay<T>, req_opt::Read> || same_as<decay<T>, req_opt::Share>;

export struct RequestOpt {
    RSTD_ENUM(RequestOpt, (Timeout, (req_opt::Timeout value;)), (Proxy, (req_opt::Proxy value;)),
              (Tcp, (req_opt::Tcp value;)), (SSL, (req_opt::SSL value;)),
              (Read, (req_opt::Read value;)), (Share, (req_opt::Share value;)))
};

export auto global_init(memory_resource* resource = nullptr) -> Result<rstd::empty>;
} // namespace ncrequest
namespace ncrequest
{

export class Request : public rstd::DefaultInClass<Request, Clone> {
public:
    Request() noexcept;
    explicit Request(lihttpto::Url url) noexcept;
    Request(Request&&) noexcept;
    ~Request() noexcept;
    Request& operator=(Request&&) noexcept;

    [[nodiscard]]
    static auto from_url(rstd::ref<rstd::str>) -> rstd::Result<Request, lihttpto::UrlError>;

    auto url() const -> std::string_view;
    auto url_info() const -> const lihttpto::Url&;
    auto try_set_url(rstd::ref<rstd::str>) -> rstd::Result<rstd::empty, lihttpto::UrlError>;

    auto header() const -> const lihttpto::Headers&;
    auto header(std::string_view name) const -> std::string;
    auto update_header(const lihttpto::Headers&) -> Request&;
    auto try_set_header(rstd::ref<rstd::str> name, rstd::ref<rstd::str> value)
        -> rstd::Result<rstd::empty, lihttpto::HeaderError>;
    auto remove_header(rstd::ref<rstd::str> name) -> Request&;
    void set_opt(const lihttpto::Headers&);

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
        m_opts.template get<decay<T>>() = rstd::forward<T>(opt);
        return *this;
    }

    auto clone() const -> ncrequest::Request;

private:
    lihttpto::Url     m_url;
    lihttpto::Headers m_header;
    RequestOpts       m_opts;
};

} // namespace ncrequest

static_assert(rstd::Impled<ncrequest::Request, Clone>);
static_assert(rstd::Impled<ncrequest::req_opt::Share, Clone>);
static_assert(rstd::Impled<ncrequest::req_opt::Proxy, Clone>);
