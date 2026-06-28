export module ncrequest:session_share;
export import ncrequest.type;
export import cppstd;

namespace ncrequest
{
export class SessionShare : public rstd::DefaultInClass<SessionShare, rstd::clone::Clone> {
public:
    class Private;
    SessionShare();
    ~SessionShare();

    auto handle() const -> voidp;
    void load(const std::filesystem::path& p);
    void save(const std::filesystem::path& p) const;
    auto clone() const -> SessionShare;

private:
    Arc<Private>          d_ptr;
    inline Private*       d_func() { return d_ptr.get(); }
    inline const Private* d_func() const { return d_ptr.get(); }
};
} // namespace ncrequest

export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::SessionShare>
    : rstd::LinkClassRequiredWithDefault<rstd::clone::Clone, ncrequest::SessionShare> {};
