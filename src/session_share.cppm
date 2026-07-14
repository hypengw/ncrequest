export module ncrequest:session_share;
export import ncrequest.type;
export import cppstd;

namespace ncrequest
{
export namespace detail
{
class SessionShareAccess;
}

export class SessionShare : public rstd::DefaultInClass<SessionShare, rstd::clone::Clone> {
public:
    SessionShare();
    ~SessionShare();
    SessionShare(SessionShare&&) noexcept;
    auto operator=(SessionShare&&) noexcept -> SessionShare&;

    SessionShare(const SessionShare&) = delete;
    auto operator=(const SessionShare&) -> SessionShare& = delete;

    void load(const std::filesystem::path& p);
    void save(const std::filesystem::path& p) const;
    auto clone() const -> SessionShare;

private:
    class Private;
    friend class detail::SessionShareAccess;

    explicit SessionShare(Arc<Private> state);

    Arc<Private> d_ptr;
};

static_assert(rstd::Impled<SessionShare, rstd::clone::Clone>);
} // namespace ncrequest
