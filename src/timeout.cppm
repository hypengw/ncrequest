export module ncrequest:timeout;
export import :error;

using namespace rstd::prelude;
using rstd::time::Duration;

namespace ncrequest
{

export class TimeoutLimit {
public:
    enum class Mode
    {
        BackendDefault,
        Disabled,
        After
    };

    static auto backend_default() -> TimeoutLimit { return {}; }
    static auto disabled() -> TimeoutLimit {
        auto out  = TimeoutLimit {};
        out.mode_ = Mode::Disabled;
        return out;
    }
    static auto after(Duration duration) -> TimeoutLimit {
        auto out      = TimeoutLimit {};
        out.mode_     = Mode::After;
        out.duration_ = duration;
        return out;
    }
    auto mode() const -> Mode { return mode_; }
    auto duration() const -> Option<Duration> {
        if (mode_ != Mode::After) return None();
        return Some(Duration(duration_));
    }

private:
    Mode     mode_ { Mode::BackendDefault };
    Duration duration_ {};
};

export struct LowSpeedOptions {
    u64      bytes_per_second;
    Duration window;
};

export struct TimeoutOptions {
    TimeoutLimit            connect { TimeoutLimit::backend_default() };
    TimeoutLimit            total { TimeoutLimit::disabled() };
    Option<LowSpeedOptions> low_speed;

    auto validate() const -> Result<empty> {
        auto valid = [](Duration duration) {
            return ! duration.is_zero() && duration.subsec_nanos() < u32(1'000'000'000);
        };
        if (auto value = connect.duration(); value.is_some() && ! valid(*value))
            return Err(Error::InvalidState("connect timeout must be a positive Duration"));
        if (auto value = total.duration(); value.is_some() && ! valid(*value))
            return Err(Error::InvalidState("total timeout must be a positive Duration"));
        if (low_speed.is_some() &&
            (low_speed->bytes_per_second == u64() || ! valid(low_speed->window)))
            return Err(
                Error::InvalidState("low-speed limit requires a positive rate and Duration"));
        return Ok(empty {});
    }
};

} // namespace ncrequest
