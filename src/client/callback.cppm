export module ncrequest:client_callback;
export import ncrequest.type;

namespace ncrequest::client
{

export template<typename Signature>
class Callback;

template<typename R, typename... Args>
class Callback<R(Args...)> {
    using Function = Box<rstd::dyn<rstd::FnMut<R(Args...)>>>;
    using State    = rstd::sync::Mutex<Function>;

    Option<Arc<State>> m_state;

public:
    Callback() = default;

    template<typename F>
        requires(! rstd::mtp::same_as<rstd::mtp::rm_cvf<F>, Callback>)
    Callback(F function)
        : m_state(Some(Arc<State>::make(Function::make(rstd::move(function))))) {}

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
