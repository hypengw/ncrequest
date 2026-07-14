module;
#include <concepts>

export module ncrequest:client_http_backend;
export import :request;
export import :http;
export import :error;
export import ncrequest.coro;
export import ncrequest.type;

namespace ncrequest::client
{

export template<typename T>
concept HttpResponseBackend = requires(T response, const T const_response) {
    { response.bytes() } -> std::same_as<coro<Result<rstd::bytes::Bytes>>>;
    { const_response.header() } -> std::same_as<const http::Header&>;
    { const_response.head() } -> std::same_as<rstd::Option<rstd::ref<http::MessageHead>>>;
    { const_response.trailers() } -> std::same_as<rstd::Option<rstd::ref<http::Header>>>;
    { const_response.request() } -> std::same_as<const Request&>;
    { const_response.operation() } -> std::same_as<http::Operation>;
    { response.cancel() } -> std::same_as<void>;
    { const_response.is_finished() } -> std::convertible_to<bool>;
};

export template<typename T, typename ResponseT>
concept HttpSessionBackend = HttpResponseBackend<ResponseT> && requires(
    T session,
    const Request& request,
    http::Operation operation,
    rstd::Option<rstd::bytes::Bytes> body,
    const req_opt::Proxy& proxy) {
    { session.start_request(request, operation, rstd::move(body)) } -> std::same_as<coro<Result<ResponseT>>>;
    { session.set_proxy(proxy) } -> std::same_as<void>;
    { session.set_verify_certificate(true) } -> std::same_as<void>;
};

} // namespace ncrequest::client
