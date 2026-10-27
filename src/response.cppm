export module ncrequest:response;
export import :request;
using namespace rstd::prelude;
namespace ncrequest
{
struct BodyState;
}
export namespace ncrequest
{
class Response;
class PendingResponse;
class ResponseBody {
    friend class Response;
    Arc<BodyState> state_;
    explicit ResponseBody(Arc<BodyState> state);

public:
    using Error = ncrequest::Error;
    ResponseBody(ResponseBody&&) noexcept;
    ResponseBody(const ResponseBody&) = delete;
    auto operator=(ResponseBody&&) noexcept -> ResponseBody&;
    ~ResponseBody();
    auto next() -> coro<Result<Option<Bytes>>>;
    auto collect() -> coro<Result<Bytes>>;
    auto collect(usize limit) -> coro<Result<Bytes>>;
    auto text() -> coro<Result<String>>;
    auto text(usize limit) -> coro<Result<String>>;
    auto json() -> coro<Result<rstd::json::Value>>;
    auto json(usize limit) -> coro<Result<rstd::json::Value>>;
    auto trailers() const -> Result<Option<lihttpto::Headers>>;
    auto completion() const -> lifetchto::TransferControl;
    void cancel() const;
    auto close() const -> coro<Arc<lifetchto::TransferReport>>;
};
struct ResponseTiming {
    rstd::time::Instant         started;
    Option<rstd::time::Instant> head_received;
};
class Response {
    friend class PendingResponse;
    struct ConstructionKey {};
    lihttpto::ResponseHead                  head_;
    lihttpto::RequestParts                  request_;
    ResponseTiming                          timing_;
    lifetchto::http::CacheObservation       cache_;
    lifetchto::http::ExecutionObserver      execution_;
    lifetchto::TransferControl              completion_;
    rstd::sync::Mutex<Option<ResponseBody>> body_;
    static auto make(lifetchto::Transfer<lifetchto::http::ResponseMessage>,
                     lifetchto::http::ResponseMessage, lifetchto::http::ExecutionObserver, usize)
        -> Arc<Response>;
    static auto collect_bytes(ResponseBody body, Option<usize> limit) -> coro<Result<Bytes>>;
    static auto collect_text(ResponseBody body, Option<usize> limit) -> coro<Result<String>>;
    static auto collect_json(ResponseBody body, Option<usize> limit)
        -> coro<Result<rstd::json::Value>>;
    template<lihttpto::BodySink Sink>
    static auto transfer(ResponseBody body, Sink& sink)
        -> coro<rstd::Result<u64, lihttpto::BodyTransferError<Error, typename Sink::Error>>> {
        auto result = co_await lihttpto::transfer_body(body, sink);
        if (result.is_err()) {
            body.cancel();
            (void)co_await body.close();
        }
        co_return rstd::move(result);
    }
    template<class SinkError>
    static auto failed_transfer(Error error)
        -> coro<rstd::Result<u64, lihttpto::BodyTransferError<Error, SinkError>>> {
        co_return Err(lihttpto::BodyTransferError<Error, SinkError>::Source(rstd::move(error)));
    }

public:
    Response(ConstructionKey, lihttpto::ResponseHead, lihttpto::RequestParts, ResponseTiming,
             lifetchto::http::CacheObservation, lifetchto::http::ExecutionObserver, ResponseBody);
    Response(const Response&) = delete;
    auto head() const -> const lihttpto::ResponseHead& { return head_; }
    auto header() const -> const lihttpto::Headers& { return head_.headers; }
    auto request() const -> const lihttpto::RequestParts& { return request_; }
    auto timing() const -> ResponseTiming { return timing_; }
    auto from_cache() const -> bool { return cache_.from_cache(); }
    auto cache() const -> lifetchto::http::CacheObservation { return cache_; }
    auto execution() const -> lifetchto::http::ExecutionObserver { return execution_.clone(); }
    auto completion() const -> lifetchto::TransferControl { return completion_.clone(); }
    void cancel() const { completion_.cancel(); }
    auto close() const -> coro<Arc<lifetchto::TransferReport>> { return completion_.close(); }
    auto take_body() -> Result<ResponseBody>;
    auto trailers() const -> Result<Option<lihttpto::Headers>>;
    auto bytes() -> coro<Result<Bytes>>;
    auto bytes(usize) -> coro<Result<Bytes>>;
    auto text() -> coro<Result<String>>;
    auto text(usize) -> coro<Result<String>>;
    auto json() -> coro<Result<rstd::json::Value>>;
    auto json(usize) -> coro<Result<rstd::json::Value>>;
    template<lihttpto::BodySink Sink>
    auto read_to_stream(Sink& sink)
        -> coro<rstd::Result<u64, lihttpto::BodyTransferError<Error, typename Sink::Error>>> {
        auto body = take_body();
        if (body.is_err())
            return failed_transfer<typename Sink::Error>(rstd::move(body).unwrap_err());
        return transfer(rstd::move(body).unwrap(), sink);
    }
    auto set_cookies() const -> rstd::Result<Vec<lihttpto::SetCookie>, lihttpto::CookieError>;
};
} // namespace ncrequest
