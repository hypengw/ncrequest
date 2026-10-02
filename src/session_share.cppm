export module ncrequest:session_share;
export import rstd;
export import :error;

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
    static auto make() -> Result<SessionShare>;
    ~SessionShare();
    SessionShare(SessionShare&&) noexcept;
    auto operator=(SessionShare&&) noexcept -> SessionShare&;

    SessionShare(const SessionShare&)                    = delete;
    auto operator=(const SessionShare&) -> SessionShare& = delete;

    auto load(ref<Path> path) -> Result<empty>;
    auto save(ref<Path> path) const -> Result<empty>;
    auto clone() const -> SessionShare;

private:
    class Private;
    friend class detail::SessionShareAccess;

    explicit SessionShare(Arc<Private> state);

    auto import_cookies(slice<u8>) -> Result<empty>;
    auto export_cookies() const -> Result<Vec<u8>>;

    Arc<Private> d_ptr;
};

static_assert(Impled<SessionShare, Clone>);
} // namespace ncrequest
