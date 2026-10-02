export module ncrequest:client_http_backend;
export import :request;
export import lihttpto;
export import :error;
export import ncrequest.coro;
export import rstd;

using namespace rstd::prelude;
using rstd::bytes::Bytes;
using rstd::mtp::convertible_to;
using rstd::mtp::same_as;

namespace ncrequest::client
{

export template<typename T>
concept HttpResponseBackend = requires(T response, const T const_response) {
    { response.next_chunk() } -> same_as<coro<Result<Option<Bytes>>>>;
    { response.ready_head() } -> same_as<coro<Result<empty>>>;
    { const_response.header() } -> same_as<const lihttpto::Headers&>;
    { const_response.head() } -> same_as<Option<ref<lihttpto::MessageHead>>>;
    { const_response.trailers() } -> same_as<Option<ref<lihttpto::Headers>>>;
    { const_response.request() } -> same_as<const Request&>;
    { response.cancel() } -> same_as<void>;
    { const_response.is_finished() } -> convertible_to<bool>;
};

export template<typename T, typename ResponseT>
concept HttpSessionBackend =
    HttpResponseBackend<ResponseT> && requires(T session, PreparedRequest request) {
        { T::initialize() } -> same_as<Result<empty>>;
        { session.start() } -> same_as<Result<empty>>;
        { session.start_request(rstd::move(request)) } -> same_as<coro<Result<ResponseT>>>;
    };

} // namespace ncrequest::client
