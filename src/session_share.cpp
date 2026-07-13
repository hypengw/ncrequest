module;
#include "macro.hpp"
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

SessionShare::SessionShare(): d_ptr(make_arc<Private>()) {
    C_D(SessionShare);
    curl_share_setopt(
        d->share, CURLSHoption::CURLSHOPT_SHARE, curl_lock_data::CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(d->share, CURLSHoption::CURLSHOPT_LOCKFUNC, Private::lock);
    curl_share_setopt(d->share, CURLSHoption::CURLSHOPT_UNLOCKFUNC, Private::unlock);
    curl_share_setopt(d->share, CURLSHoption::CURLSHOPT_USERDATA, d);
}
SessionShare::~SessionShare() {}

auto detail::SessionShareAccess::curl_handle(const SessionShare& share) -> CURLSH* {
    return share.d_ptr->share;
}
auto SessionShare::clone() const -> SessionShare { return *this; }

void SessionShare::load(const std::filesystem::path& p) {
    C_D(SessionShare);
    CurlEasy x;
    x.setopt(CURLoption::CURLOPT_SHARE, d->share);
    // append filename
    x.setopt(CURLoption::CURLOPT_COOKIEFILE, p.c_str());
    // actually load
    x.setopt(CURLoption::CURLOPT_COOKIELIST, "RELOAD");
}

void SessionShare::save(const std::filesystem::path& p) const {
    C_D(const SessionShare);
    CurlEasy x;
    x.setopt(CURLoption::CURLOPT_SHARE, d->share);
    x.setopt(CURLoption::CURLOPT_COOKIEJAR, p.c_str());
    // save when x destruct
}

} // namespace ncrequest
