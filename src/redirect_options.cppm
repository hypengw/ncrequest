export module ncrequest:redirect_options;
export import :error;

using namespace rstd::prelude;

namespace ncrequest
{

export class RedirectOptions {
public:
    static auto disabled() -> RedirectOptions { return {}; }
    static auto same_origin(u32 max_hops = u32(10)) -> RedirectOptions {
        auto out      = RedirectOptions {};
        out.enabled_  = true;
        out.max_hops_ = max_hops;
        return out;
    }
    auto enabled() const -> bool { return enabled_; }
    auto max_hops() const -> u32 { return max_hops_; }

private:
    bool enabled_ { false };
    u32  max_hops_ {};
};

} // namespace ncrequest
