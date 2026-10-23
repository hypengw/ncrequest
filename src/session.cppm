export module ncrequest:session;
export import :response;
export import :request_control;
using namespace rstd::prelude;
namespace ncrequest
{
struct PendingState;
struct SessionState;
} // namespace ncrequest
export namespace ncrequest
{
class PendingResponse {
    friend class Session;
    Arc<PendingState> state_;
    explicit PendingResponse(Arc<PendingState> state);
    static auto receive(Arc<PendingState>) -> coro<Result<Arc<Response>>>;
    void        abandon() const;

public:
    PendingResponse(PendingResponse&&) noexcept;
    PendingResponse(const PendingResponse&) = delete;
    auto operator=(PendingResponse&&) noexcept -> PendingResponse&;
    ~PendingResponse();
    auto response() const -> coro<Result<Arc<Response>>>;
    auto completion() const -> lifetchto::TransferControl;
    void cancel() const;
    auto close() const -> coro<Arc<lifetchto::TransferReport>>;
};
using Submission = rstd::Result<PendingResponse, lifetchto::Rejected<Request>>;
class Session {
    struct ConstructionKey {};
    Arc<SessionState> state_;

public:
    Session(ConstructionKey, Arc<SessionState> state);
    Session(const Session&) = delete;
    ~Session();
    static auto make(rstd::async::RuntimeHandle, lifetchto::http::TransportOptions = {},
                     const SessionOptions& = {}) -> Result<Arc<Session>>;
    static auto from_client(lifetchto::http::Client, ResponseOptions = {}) -> Result<Arc<Session>>;
    static auto from_backend(lifetchto::EngineHandle, const lifetchto::http::BackendBinding&,
                             const SessionOptions& = {}) -> Result<Arc<Session>>;
    auto        send(Request, Option<Arc<RequestControl>> = {}) const -> Submission;
    auto        get(Request, Option<Arc<RequestControl>> = {}) const -> Submission;
    auto        post(Request, Option<Arc<RequestControl>> = {}) const -> Submission;
    auto        post(Request, Bytes, Option<Arc<RequestControl>> = {}) const -> Submission;
    void        request_close() const;
    auto        close() const -> coro<lifetchto::CloseReport>;
};
} // namespace ncrequest
