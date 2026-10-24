module;
#include <QThread>
module ncrequest;
import :client.qt_network.callback_executor;
import :client.qt_network.dispatch;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client::qt_network
{
struct ExecutorFields {
    bool                          closed {};
    Vec<rstd::async::ExecutorJob> jobs;
};
struct CallbackExecutorState {
    rstd::sync::Mutex<ExecutorFields> fields { ExecutorFields {} };
    Dispatch                          dispatch;
    void                              close() {
        Vec<rstd::async::ExecutorJob> jobs;
        {
            auto f    = fields.lock().unwrap();
            f->closed = true;
            jobs      = rstd::move(f->jobs);
        }
        dispatch.close();
    }
    void advance() {
        Vec<rstd::async::ExecutorJob> jobs;
        {
            auto f = fields.lock().unwrap();
            jobs   = rstd::move(f->jobs);
        }
        for (auto& job : jobs) {
            bool closed;
            {
                auto f = fields.lock().unwrap();
                closed = f->closed;
            }
            if (! closed) job.run();
        }
    }
};
CallbackExecutor::CallbackExecutor(Arc<CallbackExecutorState> state): state_(rstd::move(state)) {}
CallbackExecutor::CallbackExecutor(CallbackExecutor&&) noexcept                    = default;
auto CallbackExecutor::operator=(CallbackExecutor&&) noexcept -> CallbackExecutor& = default;
CallbackExecutor::~CallbackExecutor()                                              = default;
auto CallbackExecutor::clone() const -> CallbackExecutor {
    return CallbackExecutor(state_.clone());
}
auto CallbackExecutor::make(QObject* receiver) -> lifetchto::Result<CallbackExecutor> {
    if (! receiver || receiver->thread() != QThread::currentThread())
        return Err(lifetchto::Error::InvalidOptions(
            "create callback executor on its receiver thread"_Str));
    auto state = Arc<CallbackExecutorState>::make();
    state->dispatch.bind(receiver,
                         Callback<void(Vec<usize>)>([state = state.clone()](Vec<usize>) {
                             state->advance();
                         }),
                         Callback<void()>([state = state.clone()] {
                             state->close();
                         }));
    return Ok(CallbackExecutor(rstd::move(state)));
}
auto CallbackExecutor::post_job(rstd::async::ExecutorJob job) const -> bool {
    {
        auto f = state_->fields.lock().unwrap();
        if (f->closed) return false;
        f->jobs.push(rstd::move(job));
    }
    state_->dispatch.notify();
    return true;
}
auto CallbackExecutor::is_closed() const -> bool { return state_->fields.lock().unwrap()->closed; }
void CallbackExecutor::close() const { state_->close(); }
} // namespace ncrequest::client::qt_network
