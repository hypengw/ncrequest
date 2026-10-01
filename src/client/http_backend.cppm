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
    { response.next_chunk() } -> rstd::mtp::same_as<coro<Result<rstd::Option<rstd::bytes::Bytes>>>>;
    { response.ready_head() } -> rstd::mtp::same_as<coro<Result<rstd::empty>>>;
    { const_response.header() } -> rstd::mtp::same_as<const lihttpto::Headers&>;
    { const_response.head() } -> rstd::mtp::same_as<rstd::Option<rstd::ref<lihttpto::MessageHead>>>;
    { const_response.trailers() } -> rstd::mtp::same_as<rstd::Option<rstd::ref<lihttpto::Headers>>>;
    { const_response.request() } -> rstd::mtp::same_as<const Request&>;
    { const_response.operation() } -> rstd::mtp::same_as<Operation>;
    { response.cancel() } -> rstd::mtp::same_as<void>;
    { const_response.is_finished() } -> rstd::mtp::convertible_to<bool>;
};

export template<typename T, typename ResponseT>
concept HttpSessionBackend =
    HttpResponseBackend<ResponseT> &&
    requires(T session, const Request& request, Operation operation,
             rstd::Option<rstd::bytes::Bytes> body, const req_opt::Proxy& proxy) {
        {
            session.start_request(request, operation, rstd::move(body))
        } -> rstd::mtp::same_as<coro<Result<ResponseT>>>;
        { session.set_proxy(proxy) } -> rstd::mtp::same_as<void>;
        { session.set_verify_certificate(true) } -> rstd::mtp::same_as<void>;
    };

} // namespace ncrequest::client
