#pragma once

#include <string_view>
#include <map>
#include <variant>
#include <vector>
#include <cstdint>
#include <memory>

namespace ncrequest
{

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using idx         = std::ptrdiff_t;
using usize       = std::size_t;
using isize       = std::ptrdiff_t;
using byte        = std::byte;
using voidp       = void*;
using const_voidp = const void*;

template<typename T>
using rc = std::shared_ptr<T>;

template<typename T>
using weak = std::weak_ptr<T>;

template<typename T, typename D = std::default_delete<T>>
using up = std::unique_ptr<T, D>;

struct NoCopy {
protected:
    NoCopy()  = default;
    ~NoCopy() = default;

    NoCopy(const NoCopy&)            = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};

template<class T, template<class...> class Primary>
struct is_specialization_of : std::false_type {};
template<template<class...> class Primary, class... Args>
struct is_specialization_of<Primary<Args...>, Primary> : std::true_type {};
template<class T, template<class...> class Primary>
inline constexpr bool is_specialization_of_v = is_specialization_of<T, Primary>::value;

enum class Attribute
{
    HttpCode
};

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

template<Attribute A>
using attr_type = typename detail::attr_type<A>::type;

using attr_value = std::variant<std::monostate, i32, bool>;

enum class Operation
{
    GetOperation    = 0,
    PostOperation   = 1,
    DeleteOperation = 2,
    HeadOperation   = 3,
    GET             = GetOperation,
    POST            = PostOperation,
    DELETE          = DeleteOperation,
    HEAD            = HeadOperation,
};

struct CaseInsensitiveCompare {
    using is_transparent = void;
    bool operator()(std::string_view, std::string_view) const noexcept;
};

using Header = std::map<std::string, std::string, CaseInsensitiveCompare>;

class UrlParams {
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

std::string url_encode(std::string_view);
std::string url_decode(std::string_view);

struct Cookie {};
struct CookieJar {
    std::string raw_cookie;
};

class Connection;
namespace session_message
{
struct Stop {};

struct ConnectAction {
    enum class Action
    {
        Add,
        Cancel,
        PauseRecv,
        UnPauseRecv,
        PauseSend,
        UnPauseSend,
    };
    rc<Connection> con;
    Action         action;
};

using msg = std::variant<Stop, ConnectAction>;
} // namespace session_message

using SessionMessage = session_message::msg;

} // namespace ncrequest
