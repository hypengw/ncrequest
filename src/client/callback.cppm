export module ncrequest:client.callback;
export import rstd;

using namespace rstd::prelude;
using rstd::mtp::rm_cvf;
using rstd::mtp::same_as;
using rstd::sync::Arc;
using rstd::sync::Mutex;

namespace ncrequest::client
{

export template<typename Signature>
class Callback;

template<typename R, typename... Args>
class Callback<R(Args...)> {
    using Function = Box<dyn<FnMut<R(Args...)>>>;
    using State    = Mutex<Function>;

    Option<Arc<State>> m_state;

public:
    Callback() = default;

    template<typename F>
        requires(! same_as<rm_cvf<F>, Callback>)
    Callback(F function): m_state(Some(Arc<State>::make(Function::make(rstd::move(function))))) {}

    Callback(const Callback&)                    = delete;
    auto operator=(const Callback&) -> Callback& = delete;
    Callback(Callback&&)                         = default;
    auto operator=(Callback&&) -> Callback&      = default;

    explicit operator bool() const noexcept { return m_state.is_some(); }

    auto clone() const -> Callback {
        auto result = Callback {};
        if (m_state.is_some()) result.m_state = Some(m_state->clone());
        return result;
    }

    auto operator()(Args... args) -> R {
        if (m_state.is_none()) rstd::panic { "empty client callback invoked" };

        auto state    = m_state->clone();
        auto function = state->lock().unwrap();
        return (*function)->operator()(rstd::forward<Args>(args)...);
    }
};

} // namespace ncrequest::client
