module;
#include <QCoreApplication>
#include <QEvent>
#include <QMutex>
#include <QWaitCondition>
module ncrequest;
import :client.qt_network.dispatch;
using namespace rstd::prelude;
namespace ncrequest::client::qt_network
{
struct DispatchState {
    QMutex         gate;
    QWaitCondition posting;
    QObject*       target {};
    usize          posts {}, next { 1 };
    bool           closed {}, pending {};
    Vec<usize>     ready;
};
auto dispatch_type() -> QEvent::Type {
    static auto type = QEvent::Type(QEvent::registerEventType());
    return type;
}
void close_dispatch(const Arc<DispatchState>& state) {
    QMutexLocker lock(&state->gate);
    state->closed = true;
    state->target = nullptr;
    while (state->posts != usize()) state->posting.wait(&state->gate);
    state->ready.clear();
}
class DispatchObject : public QObject {
    Arc<DispatchState>         state_;
    Callback<void(Vec<usize>)> advance_;
    Callback<void()>           release_;

public:
    DispatchObject(QObject* parent, Arc<DispatchState> state, Callback<void(Vec<usize>)> advance,
                   Callback<void()> release)
        : QObject(parent),
          state_(rstd::move(state)),
          advance_(rstd::move(advance)),
          release_(rstd::move(release)) {}
    ~DispatchObject() {
        close_dispatch(state_);
        if (release_) release_();
    }
    bool event(QEvent* event) override {
        if (event->type() != dispatch_type()) return QObject::event(event);
        Vec<usize> ready;
        bool       closed;
        {
            QMutexLocker lock(&state_->gate);
            state_->pending = false;
            ready           = rstd::move(state_->ready);
            closed          = state_->closed;
        }
        if (! closed) advance_(rstd::move(ready));
        return true;
    }
};
Dispatch::Dispatch(Dispatch&&) noexcept                    = default;
auto Dispatch::operator=(Dispatch&&) noexcept -> Dispatch& = default;
Dispatch::~Dispatch()                                      = default;
Dispatch::Dispatch(): state_(Arc<DispatchState>::make()) {}
Dispatch::Dispatch(Arc<DispatchState> state, usize id): state_(rstd::move(state)), id_(id) {}
auto Dispatch::clone() const -> Dispatch { return Dispatch(state_.clone(), id_); }
auto Dispatch::operation() const -> Dispatch {
    QMutexLocker lock(&state_->gate);
    return Dispatch(state_.clone(), state_->next++);
}
void Dispatch::notify() const {
    QObject* target {};
    {
        QMutexLocker lock(&state_->gate);
        if (state_->closed) return;
        bool exists = false;
        for (auto id : state_->ready) exists = exists || id == id_;
        if (! exists) state_->ready.push(usize(id_));
        if (state_->target && ! state_->pending) {
            state_->pending = true;
            target          = state_->target;
            ++state_->posts;
        }
    }
    if (! target) return;
    QCoreApplication::postEvent(target, new QEvent(dispatch_type()));
    {
        QMutexLocker lock(&state_->gate);
        --state_->posts;
        state_->posting.wakeAll();
    }
}
void Dispatch::bind(QObject* owner, Callback<void(Vec<usize>)> advance,
                    Callback<void()> release) const {
    auto* target =
        new DispatchObject(owner, state_.clone(), rstd::move(advance), rstd::move(release));
    {
        QMutexLocker lock(&state_->gate);
        if (! state_->closed) state_->target = target;
    }
    notify();
}
void Dispatch::close() const { close_dispatch(state_); }
} // namespace ncrequest::client::qt_network
