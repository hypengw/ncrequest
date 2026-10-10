export module ncrequest:request_control;
import :client.callback;
export import rstd;

using namespace rstd::prelude;
using rstd::sync::Mutex;
using rstd::sync::atomic::Atomic;

namespace ncrequest::client::curl
{
class SessionBackend;
}

namespace ncrequest
{

export class Session;

export class RequestControl {
    friend class Session;
    friend class client::curl::SessionBackend;
    using Cancel = client::Callback<void()>;
    struct State {
        bool   canceled { false };
        Cancel cancel;
    };
    Mutex<State> state_ { State {} };
    Atomic<bool> claimed_ { false };

    auto claim() -> bool { return ! claimed_.exchange(true); }

    void bind(Cancel callback) {
        bool canceled;
        {
            auto state = state_.lock().unwrap();
            canceled   = state->canceled;
            if (! canceled) state->cancel = rstd::move(callback);
        }
        if (canceled) callback();
    }

public:
    RequestControl()                                         = default;
    RequestControl(const RequestControl&)                    = delete;
    auto operator=(const RequestControl&) -> RequestControl& = delete;

    void cancel() {
        Cancel callback;
        {
            auto state = state_.lock().unwrap();
            if (state->canceled) return;
            state->canceled = true;
            callback        = rstd::move(state->cancel);
        }
        if (callback) callback();
    }

    auto is_canceled() const -> bool { return state_.lock().unwrap()->canceled; }
};

} // namespace ncrequest
