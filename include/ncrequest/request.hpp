#pragma once

#include <string_view>

#include "ncrequest/uri.hpp"
#include "ncrequest/type.hpp"
#include "ncrequest/request_opt.hpp"
#include "ncrequest/type_list.hpp"

namespace ncrequest
{

std::error_code global_init();

class Session;
class Request {
    friend class Session;
    friend class Response;

public:
    class Private;
    Request() noexcept;
    Request(std::string_view url) noexcept;
    ~Request() noexcept;

    Request(const Request&);
    Request& operator=(const Request&);

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
    const_voidp           get_opt(usize) const;
    voidp                 get_opt(usize);
    up<Private>           m_d;
    inline Private*       d_func() { return m_d.get(); }
    inline const Private* d_func() const { return m_d.get(); }
};

} // namespace ncrequest
