export module ncrequest:options;
export import :error;
using namespace rstd::prelude;
export namespace ncrequest
{
struct ResponseOptions {
    usize collect_bytes { 8 * 1024 * 1024 };
};
struct RequestOptions {
    lifetchto::http::ExecuteOptions execution;
    Option<usize>                   collect_bytes;
    auto clone() const -> RequestOptions { return { execution.clone(), collect_bytes }; }
};
struct SessionOptions {
    lifetchto::http::ClientConfig http;
    ResponseOptions               response;
};
} // namespace ncrequest
