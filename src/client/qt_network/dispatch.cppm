module;
#include <QObject>
module ncrequest:client.qt_network.dispatch;
import :client.callback;
using namespace rstd::prelude;
namespace ncrequest::client::qt_network
{
struct DispatchState;
class Dispatch {
    Arc<DispatchState> state_;
    usize              id_;
    Dispatch(Arc<DispatchState>, usize);

public:
    Dispatch();
    Dispatch(Dispatch&&) noexcept;
    auto operator=(Dispatch&&) noexcept -> Dispatch&;
    ~Dispatch();
    auto clone() const -> Dispatch;
    auto operation() const -> Dispatch;
    auto id() const -> usize { return id_; }
    void notify() const;
    void bind(QObject*, Callback<void(Vec<usize>)>, Callback<void()> = {}) const;
    void close() const;
};
} // namespace ncrequest::client::qt_network
