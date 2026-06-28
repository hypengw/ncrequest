module;
#include <string>
#include <variant>

export module ncrequest:response;

#if defined(NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
export import :client_qt_network;
#else
export import :client_curl_response;
#endif
export import :client_http_backend;

namespace ncrequest
{

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

    auto code() const -> rstd::Option<i32> {
        auto& start = this->header().start;
        if (start) {
            if (auto* status = std::get_if<HttpHeader::Status>(&*start)) {
                return Some<i32>(status->code);
            }
        }
        return None<i32>();
    }

    auto text() -> coro<Result<std::string>> {
        auto data_result = co_await this->bytes();
        if (data_result.is_err()) {
            auto err = rstd::move(data_result).unwrap_err();
            co_return Result<std::string>(Err(rstd::move(err)));
        }

        auto        data = rstd::move(data_result).unwrap();
        std::string out;
        out.assign(reinterpret_cast<const char*>(data.data()), data.size());
        co_return Result<std::string>(Ok(rstd::move(out)));
    }
};

} // namespace ncrequest
