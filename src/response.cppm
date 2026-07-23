module;
#include <string>

export module ncrequest:response;

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
export import :client_qt_network;
#else
export import :client_curl_response;
#endif
export import :client_http_backend;

namespace ncrequest
{

using namespace rstd::literals;

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
using SelectedResponseBackend = client::qt_network::ResponseBackend;
#else
using SelectedResponseBackend = client::curl::ResponseBackend;
#endif

static_assert(client::HttpResponseBackend<SelectedResponseBackend>);

export class Response : public SelectedResponseBackend {
public:
    using Backend = SelectedResponseBackend;

    explicit Response(Backend&& backend): Backend(rstd::move(backend)) {}

    auto code() const -> rstd::Option<i32> { return Backend::code(); }

    auto set_cookies() const -> rstd::Result<rstd::vec::Vec<http::SetCookie>, http::CookieError> {
        auto cookies = rstd::vec::Vec<http::SetCookie>::make();
        auto values  = this->header().values("set-cookie"_str);
        for (auto value = values.next(); value.is_some(); value = values.next()) {
            auto parsed = http::SetCookie::parse_bytes((**value).as_bytes());
            if (parsed.is_err()) {
                return rstd::Err(rstd::move(parsed).unwrap_err());
            }
            cookies.push(rstd::move(parsed).unwrap());
        }
        return rstd::Ok(rstd::move(cookies));
    }

    auto text() -> coro<Result<std::string>> {
        auto data_result = co_await this->bytes();
        if (data_result.is_err()) {
            auto err = rstd::move(data_result).unwrap_err();
            co_return Result<std::string>(Err(rstd::move(err)));
        }

        auto        data = rstd::move(data_result).unwrap();
        std::string out;
        out.assign(reinterpret_cast<const char*>(data.data()), data.size().to_primitive());
        co_return Result<std::string>(Ok(rstd::move(out)));
    }
};

} // namespace ncrequest
