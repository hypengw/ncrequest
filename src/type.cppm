module;
#include <string_view>
#include <map>
#include <variant>
#include <vector>
#include <memory>

export module ncrequest.type;
export import ncrequest.coro;
export import rstd;

namespace ncrequest
{

export using namespace rstd;

export template<typename T>
using Arc = std::shared_ptr<T>;

export template<typename T>
using Weak = std::weak_ptr<T>;

export template<typename T, typename D = std::default_delete<T>>
using Box = std::unique_ptr<T, D>;

export struct NoCopy {
protected:
    NoCopy()  = default;
    ~NoCopy() = default;

    NoCopy(NoCopy&&)            = default;
    NoCopy& operator=(NoCopy&&) = default;

    NoCopy(const NoCopy&)            = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};

export template<class T, template<class...> class Primary>
struct is_specialization_of : std::false_type {};
export template<template<class...> class Primary, class... Args>
struct is_specialization_of<Primary<Args...>, Primary> : std::true_type {};
export template<class T, template<class...> class Primary>
inline constexpr bool is_specialization_of_v = is_specialization_of<T, Primary>::value;

export enum class Attribute { HttpCode };

namespace detail
{

template<Attribute A>
struct attr_type {};

#define AT(A, T)                     \
    template<>                       \
    struct attr_type<Attribute::A> { \
        using type = T;              \
    }

AT(HttpCode, i32);

#undef AT
} // namespace detail

export template<Attribute A>
using attr_type = typename detail::attr_type<A>::type;

export using attr_value = std::variant<std::monostate, i32, bool>;

export enum class Operation {
    GetOperation    = -1,
    PostOperation   = 0,
    DeleteOperation = 1,
    HeadOperation   = 2,
    GET             = GetOperation,
    POST            = PostOperation,
    DELETE          = DeleteOperation,
    HEAD            = HeadOperation,
};

export struct CaseInsensitiveCompare {
    using is_transparent = void;
    bool operator()(std::string_view, std::string_view) const noexcept;
};

export using Header = std::map<std::string, std::string, CaseInsensitiveCompare>;

export class UrlParams {
public:
    auto param(std::string_view) const -> std::string_view;
    auto params(std::string_view) const -> std::vector<std::string_view>;
    auto is_array(std::string_view) const -> bool;
    auto set_param(std::string_view, std::string_view) -> UrlParams&;
    auto add_param(std::string_view, std::string_view) -> UrlParams&;

    template<typename T>
        requires(! std::convertible_to<T, std::string_view>) && std::ranges::range<T>
    auto set_param(std::string_view name, const T& arr) -> UrlParams& {
        if constexpr (std::same_as<std::string, T> || std::same_as<std::string_view, T>) {
            for (const auto& el : arr) add_param(name, el);
        } else {
            for (const auto& el : arr) add_param(name, convert_from<std::string>(el));
        }
        return *this;
    }
    auto set_param(std::string_view, const std::map<std::string, std::string>&) -> UrlParams& {
        return *this;
    }

    void decode(std::string_view);
    auto encode() const -> std::string;

private:
    std::map<std::string, std::vector<std::string>, std::less<>> m_params;
};

export std::string url_encode(std::string_view);
export std::string url_decode(std::string_view);

export struct Cookie {};
export struct CookieJar {
    std::string raw_cookie;
};

export template<typename T, typename... Args>
auto make_box(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}
export template<typename T, typename... Args>
auto make_arc(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

namespace helper
{

export template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
export template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

export template<typename T>
concept is_sync_stream = requires(T s, asio::const_buffer buf) {
    { s.write_some(buf) } -> std::convertible_to<std::size_t>;
};

export constexpr auto case_insensitive_compare(std::string_view a, std::string_view b) noexcept {
    return std::lexicographical_compare_three_way(
        a.begin(), a.end(), b.begin(), b.end(), [](unsigned char a, unsigned char b) {
            const auto la = std::tolower(a);
            const auto lb = std::tolower(b);
            return (la < lb)   ? std::weak_ordering::less
                   : (la > lb) ? std::weak_ordering::greater
                               : std::weak_ordering::equivalent;
        });
}

export constexpr bool starts_with_i(std::string_view str, std::string_view start) noexcept {
    return str.size() >= start.size() &&
           case_insensitive_compare({ str.begin(), str.begin() + start.size() }, start) == 0;
}

} // namespace helper
} // namespace ncrequest