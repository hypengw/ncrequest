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

    void load(const std::filesystem::path& p);
    void save(const std::filesystem::path& p) const;
    auto clone() const -> SessionShare;

private:
    class Private;
    friend class detail::SessionShareAccess;

    Arc<Private>          d_ptr;
    inline Private*       d_func() { return d_ptr.get(); }
    inline const Private* d_func() const { return d_ptr.get(); }
};
} // namespace ncrequest

export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::SessionShare>
    : rstd::LinkClassMethod<rstd::clone::Clone, ncrequest::SessionShare> {};
