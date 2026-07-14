module;
module ncrequest;
import :session_share;
import :session_share_backend;
import ncrequest.curl;

namespace ncrequest
{
using namespace curl;

class SessionShare::Private {
public:
    Private(): share(curl_share_init()) {}
    ~Private() { curl_share_cleanup(share); }

    static void lock(CURL*, curl_lock_data data, curl_lock_access, void* clientp) {
        auto* self = static_cast<Private*>(clientp);
        if (data == curl_lock_data::CURL_LOCK_DATA_COOKIE) self->share_mutex.lock();
    }

    static void unlock(CURL*, curl_lock_data data, void* clientp) {
        auto* self = static_cast<Private*>(clientp);
        if (data == curl_lock_data::CURL_LOCK_DATA_COOKIE) self->share_mutex.unlock();
    }

    CURLSH*    share;
    std::mutex share_mutex;
};

SessionShare::SessionShare(Arc<Private> state): d_ptr(rstd::move(state)) {}
SessionShare::SessionShare(SessionShare&&) noexcept = default;
auto SessionShare::operator=(SessionShare&&) noexcept -> SessionShare& = default;

SessionShare::SessionShare(): d_ptr(Arc<Private>::make()) {
    curl_share_setopt(
        d_ptr->share, CURLSHoption::CURLSHOPT_SHARE, curl_lock_data::CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(d_ptr->share, CURLSHoption::CURLSHOPT_LOCKFUNC, Private::lock);
    curl_share_setopt(d_ptr->share, CURLSHoption::CURLSHOPT_UNLOCKFUNC, Private::unlock);
    curl_share_setopt(d_ptr->share,
                      CURLSHoption::CURLSHOPT_USERDATA,
                      d_ptr.as_ptr().as_raw_ptr());
}
SessionShare::~SessionShare() {}

auto detail::SessionShareAccess::curl_handle(const SessionShare& share) -> CURLSH* {
    return share.d_ptr->share;
}
auto SessionShare::clone() const -> SessionShare { return SessionShare { d_ptr.clone() }; }

void SessionShare::load(const std::filesystem::path& p) {
    CurlEasy x;
    x.setopt(CURLoption::CURLOPT_SHARE, d_ptr->share);
    // append filename
    x.setopt(CURLoption::CURLOPT_COOKIEFILE, p.c_str());
    // actually load
    x.setopt(CURLoption::CURLOPT_COOKIELIST, "RELOAD");
}

void SessionShare::save(const std::filesystem::path& p) const {
    CurlEasy x;
    x.setopt(CURLoption::CURLOPT_SHARE, d_ptr->share);
    x.setopt(CURLoption::CURLOPT_COOKIEJAR, p.c_str());
    // save when x destruct
}

} // namespace ncrequest
