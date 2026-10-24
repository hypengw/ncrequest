module;
#include <QObject>
export module ncrequest:client.qt_network.callback_executor;
export import :websocket_backend;
using namespace rstd::prelude;
export namespace ncrequest::client::qt_network
{
struct CallbackExecutorState;
class CallbackExecutor {
    Arc<CallbackExecutorState> state_;
    explicit CallbackExecutor(Arc<CallbackExecutorState>);

public:
    CallbackExecutor(CallbackExecutor&&) noexcept;
    auto operator=(CallbackExecutor&&) noexcept -> CallbackExecutor&;
    ~CallbackExecutor();
    static auto make(QObject*) -> lifetchto::Result<CallbackExecutor>;
    auto        clone() const -> CallbackExecutor;
    auto        post_job(rstd::async::ExecutorJob) const -> bool;
    auto        is_closed() const -> bool;
    void        close() const;
};
} // namespace ncrequest::client::qt_network
export namespace rstd
{
template<>
struct Impl<async::Executor, ncrequest::client::qt_network::CallbackExecutor>
    : ImplBase<ncrequest::client::qt_network::CallbackExecutor> {
    auto post_job(async::ExecutorJob job) { return this->self().post_job(rstd::move(job)); }
    auto is_closed() { return this->self().is_closed(); }
};
} // namespace rstd
