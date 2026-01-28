export module ncrequest.curl:multi;
export import :easy;
export import :error;
export import ncrequest.type;

using namespace curl;

namespace ncrequest
{

export class CurlMulti : public NoCopy {
public:
    struct InfoMsg {
        CURLMSG  msg;
        CURL*    easy_handle;
        CURLcode result;
    };

    CurlMulti() noexcept: m_multi(curl_multi_init()), m_share(curl_share_init()) {
        // curl_multi_setopt(m_multi, CURLMOPT_SOCKETFUNCTION, CurlMulti::curl_socket_func);
        // curl_multi_setopt(m_multi, CURLMOPT_SOCKETDATA, this);
        // curl_multi_setopt(m_multi, CURLMOPT_TIMERFUNCTION, CurlMulti::curl_timer_func);
        // curl_multi_setopt(m_multi, CURLMOPT_TIMERDATA, this);

        curl_share_setopt(m_share, CURLSHoption::CURLSHOPT_SHARE, curl_lock_data::CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(m_share, CURLSHoption::CURLSHOPT_LOCKFUNC, CurlMulti::static_share_lock);
        curl_share_setopt(
            m_share, CURLSHoption::CURLSHOPT_UNLOCKFUNC, CurlMulti::static_share_unlock);
        curl_share_setopt(m_share, CURLSHoption::CURLSHOPT_USERDATA, this);
    }

    ~CurlMulti() {
        curl_multi_cleanup(m_multi);
        curl_share_cleanup(m_share);
    }

    rstd::error_code add_handle(CurlEasy& easy) {
        rstd::error_code cm {};
        if (easy.getopt<CURLoption::CURLOPT_SHARE>() == nullptr) {
            cm = easy.setopt<CURLoption::CURLOPT_SHARE>(m_share);
        }
        if (cm) return cm;
        cm = curl_multi_add_handle(m_multi, easy.handle());
        return cm;
    }

    rstd::error_code remove_handle(CurlEasy& easy) {
        return curl_multi_remove_handle(m_multi, easy.handle());
    }
    rstd::error_code remove_handle(CURL* easy) { return curl_multi_remove_handle(m_multi, easy); }

    rstd::error_code wakeup() { return curl_multi_wakeup(m_multi); }

    rstd::error_code perform(int& still_running) {
        return curl_multi_perform(m_multi, &still_running);
    }

    rstd::error_code poll(cppstd::chrono::milliseconds timeout) {
        return curl_multi_poll(m_multi, nullptr, 0, (int)timeout.count(), nullptr);
    }

    cppstd::vector<InfoMsg> query_info_msg() {
        cppstd::vector<InfoMsg> out;
        int                     message_left { 0 };
        while (CURLMsg* msg = curl_multi_info_read(m_multi, &message_left)) {
            out.push_back(InfoMsg {
                .msg         = msg->msg,
                .easy_handle = msg->easy_handle,
                .result      = msg->data.result,
            });
        }
        return out;
    }

    auto cookies() const -> cppstd::vector<cppstd::string> {
        cppstd::vector<cppstd::string> out;
        CurlEasy                       x;

        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        auto list_ = x.get_info<curl_slist*>(CURLINFO::CURLINFO_COOKIELIST);
        if (auto plist = rstd::get_if<curl_slist*>(&list_)) {
            auto list = *plist;
            while (list) {
                out.emplace_back(list->data);
                list = list->next;
            }
            curl_slist_free_all(*plist);
        }
        return out;
    }

    void load_cookie(cppstd::filesystem::path p) {
        CurlEasy x;
        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        // append filename
        x.setopt(CURLoption::CURLOPT_COOKIEFILE, p.c_str());
        // actually load
        x.setopt(CURLoption::CURLOPT_COOKIELIST, "RELOAD");
    }

    void save_cookie(cppstd::filesystem::path p) const {
        CurlEasy x;
        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        x.setopt(CURLoption::CURLOPT_COOKIEJAR, p.c_str());
        // save when x destruct
    }

private:
    static void static_share_lock(CURL*, curl_lock_data data, curl_lock_access, void* clientp) {
        auto info = static_cast<CurlMulti*>(clientp);
        if (data == curl_lock_data::CURL_LOCK_DATA_COOKIE) {
            info->m_share_mutex.lock();
        }
    }

    static void static_share_unlock(CURL*, curl_lock_data data, void* clientp) {
        auto info = static_cast<CurlMulti*>(clientp);
        if (data == curl_lock_data::CURL_LOCK_DATA_COOKIE) {
            info->m_share_mutex.unlock();
        }
    }

private:
    CURLM*  m_multi;
    CURLSH* m_share;

    cppstd::mutex m_share_mutex;
};
} // namespace ncrequest
