module;
#include <memory>
#include <mutex>

export module ncrequest.coro:rstd;
export import rstd;

namespace ncrequest::rstd_coro
{

export template<typename T>
class PollState {
    struct Fields {
        rstd::Option<T>                 value;
        rstd::Option<rstd::task::Waker> waker;
        bool                            ready { false };
        bool                            consumed { false };
        bool                            canceled { false };
    };

    std::mutex m_mutex;
    Fields     m_fields {};

public:
    void set_ready(T value) {
        auto waker = rstd::Option<rstd::task::Waker> {};
        {
            auto lock = std::lock_guard { m_mutex };
            if (m_fields.ready || m_fields.canceled) return;
            m_fields.value = rstd::Some(rstd::move(value));
            m_fields.ready = true;
            waker          = m_fields.waker.take();
        }

        if (waker.is_some()) {
            rstd::move(*waker).wake();
        }
    }

    void cancel() {
        auto lock          = std::lock_guard { m_mutex };
        m_fields.canceled  = true;
        m_fields.waker     = rstd::None();
        m_fields.value     = rstd::None();
        m_fields.ready     = false;
        m_fields.consumed  = false;
    }

    auto is_canceled() -> bool {
        auto lock = std::lock_guard { m_mutex };
        return m_fields.canceled;
    }

    auto poll(rstd::task::Context& cx) -> rstd::task::Poll<T> {
        auto lock = std::lock_guard { m_mutex };
        if (m_fields.ready) {
            if (m_fields.consumed) {
                rstd::panic { "ncrequest future polled after completion" };
            }
            m_fields.consumed = true;
            return rstd::task::Poll<T>::Ready(rstd::move(m_fields.value).unwrap_unchecked());
        }

        if (! m_fields.canceled) {
            m_fields.waker = rstd::Some(cx.waker().clone());
        }
        return rstd::task::Poll<T>::Pending();
    }
};

export template<typename T>
using PollStateArc = std::shared_ptr<PollState<T>>;

export template<typename T, typename... Args>
auto make_poll_state(Args&&... args) -> PollStateArc<T> {
    return std::make_shared<PollState<T>>(rstd::forward<Args>(args)...);
}

} // namespace ncrequest::rstd_coro
