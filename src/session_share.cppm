export module ncrequest:session_share;
export import rstd;

using namespace rstd::prelude;
using rstd::path::Path;
using rstd::sync::Arc;

namespace ncrequest
{
export namespace detail
{
class SessionShareAccess;
}

export class SessionShare : public DefaultInClass<SessionShare, Clone> {
public:
    SessionShare();
    ~SessionShare();
    SessionShare(SessionShare&&) noexcept;
    auto operator=(SessionShare&&) noexcept -> SessionShare&;

    SessionShare(const SessionShare&)                    = delete;
    auto operator=(const SessionShare&) -> SessionShare& = delete;

    void load(ref<Path> path);
    void save(ref<Path> path) const;
    auto clone() const -> SessionShare;

private:
    class Private;
    friend class detail::SessionShareAccess;

    explicit SessionShare(Arc<Private> state);

    Arc<Private> d_ptr;
};

static_assert(Impled<SessionShare, Clone>);
} // namespace ncrequest
