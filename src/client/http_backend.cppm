export module ncrequest:client_http_backend;
export import :request;
export import :http;
export import :error;
export import ncrequest.coro;
export import ncrequest.type;

using rstd::bytes::Bytes;
using rstd::mtp::convertible_to;
using rstd::mtp::same_as;

namespace ncrequest::client
{

export template<typename T>
concept HttpResponseBackend = requires(T response, const T const_response) {
    { response.next_chunk() } -> same_as<coro<Result<rstd::Option<Bytes>>>>;
    { response.ready_head() } -> same_as<coro<Result<rstd::empty>>>;
    { const_response.header() } -> same_as<const lihttpto::Headers&>;
    { const_response.head() } -> same_as<rstd::Option<rstd::ref<lihttpto::MessageHead>>>;
    { const_response.trailers() } -> same_as<rstd::Option<rstd::ref<lihttpto::Headers>>>;
    { const_response.request() } -> same_as<const Request&>;
    { const_response.operation() } -> same_as<Operation>;
    { response.cancel() } -> same_as<void>;
    { const_response.is_finished() } -> convertible_to<bool>;
};

export template<typename T, typename ResponseT>
concept HttpSessionBackend = HttpResponseBackend<ResponseT> &&
                             requires(T session, const Request& request, Operation operation,
                                      rstd::Option<Bytes> body, const req_opt::Proxy& proxy) {
                                 {
                                     session.start_request(request, operation, rstd::move(body))
                                 } -> same_as<coro<Result<ResponseT>>>;
                                 { session.set_proxy(proxy) } -> same_as<void>;
                                 { session.set_verify_certificate(true) } -> same_as<void>;
                             };

} // namespace ncrequest::client
