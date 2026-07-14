module;
#include <concepts>
#include <memory>
#include <type_traits>

export module ncrequest.type;
export import ncrequest.coro;
export import rstd;

namespace ncrequest
{

export using namespace rstd::prelude;

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

export template<typename T>
concept is_sync_stream = requires(T s, slice<u8> buf) {
    { s.write_some(buf) } -> std::convertible_to<std::size_t>;
};

} // namespace helper
} // namespace ncrequest
