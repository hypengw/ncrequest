export module ncrequest:session_share;
export import ncrequest.type;

namespace ncrequest
{
export class SessionShare : public rstd::WithTrait<SessionShare, rstd::clone::Clone> {
public:
    class Private;
    SessionShare();
    ~SessionShare();

    auto handle() const -> voidp;
    void load(const rstd::cppstd::filesystem::path& p);
    void save(const rstd::cppstd::filesystem::path& p) const;

private:
    Arc<Private>          d_ptr;
    inline Private*       d_func() { return d_ptr.get(); }
    inline const Private* d_func() const { return d_ptr.get(); }
};
} // namespace ncrequest
