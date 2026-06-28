module;
#include <chrono>

export module ncrequest.curl:multi;
export import :easy;
export import :error;
export import ncrequest.type;

using namespace curl;

namespace ncrequest
{

export struct CurlOptions {
    long max_idle_connections { 0 };
    long max_host_connections { 0 };
    long max_total_connections { 0 };
    long receive_buffer_size { 0 };
    long upload_buffer_size { 0 };
    bool enable_http_multiplex { false };
};

export class CurlMulti : public NoCopy {
public:
    struct InfoMsg {
        CURLMSG  msg;
        CURL*    easy_handle;
        CURLcode result;
    };

    CurlMulti(CurlOptions options = {}) noexcept
        : m_multi(curl_multi_init()),
          m_share(curl_share_init()),
          m_options(options) {
        // curl_multi_setopt(m_multi, CURLMOPT_SOCKETFUNCTION, CurlMulti::curl_socket_func);
        // curl_multi_setopt(m_multi, CURLMOPT_SOCKETDATA, this);
        // curl_multi_setopt(m_multi, CURLMOPT_TIMERFUNCTION, CurlMulti::curl_timer_func);
        // curl_multi_setopt(m_multi, CURLMOPT_TIMERDATA, this);

        curl_share_setopt(m_share, CURLSHoption::CURLSHOPT_SHARE, curl_lock_data::CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(m_share, CURLSHoption::CURLSHOPT_LOCKFUNC, CurlMulti::static_share_lock);
        curl_share_setopt(
            m_share, CURLSHoption::CURLSHOPT_UNLOCKFUNC, CurlMulti::static_share_unlock);
        curl_share_setopt(m_share, CURLSHoption::CURLSHOPT_USERDATA, this);
        apply_multi_options();
    }

    ~CurlMulti() {
        curl_multi_cleanup(m_multi);
        curl_share_cleanup(m_share);
    }

    std::error_code add_handle(CurlEasy& easy) {
        std::error_code cm {};
        cm = apply_easy_options(easy);
        if (cm) return cm;
        if (easy.getopt<CURLoption::CURLOPT_SHARE>() == nullptr) {
            cm = easy.setopt<CURLoption::CURLOPT_SHARE>(m_share);
        }
        if (cm) return cm;
        cm = curl_multi_add_handle(m_multi, easy.handle());
        return cm;
    }

    std::error_code set_options(CurlOptions options) {
        auto old = m_options;
        m_options = options;
        auto ec = apply_multi_options();
        if (ec) {
            m_options = old;
            return ec;
        }
        return {};
    }

    auto options() const noexcept -> const CurlOptions& { return m_options; }

    std::error_code remove_handle(CurlEasy& easy) {
        return curl_multi_remove_handle(m_multi, easy.handle());
    }
    std::error_code remove_handle(CURL* easy) { return curl_multi_remove_handle(m_multi, easy); }

    std::error_code wakeup() { return curl_multi_wakeup(m_multi); }

    std::error_code perform(int& still_running) {
        return curl_multi_perform(m_multi, &still_running);
    }

    std::error_code poll(std::chrono::milliseconds timeout) {
        return curl_multi_poll(m_multi, nullptr, 0, (int)timeout.count(), nullptr);
    }

    std::vector<InfoMsg> query_info_msg() {
        std::vector<InfoMsg> out;
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

    auto cookies() const -> std::vector<std::string> {
        std::vector<std::string> out;
        CurlEasy                       x;

        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        auto list_ = x.get_info<curl_slist*>(CURLINFO::CURLINFO_COOKIELIST);
        if (auto plist = std::get_if<curl_slist*>(&list_)) {
            auto list = *plist;
            while (list) {
                out.emplace_back(list->data);
                list = list->next;
            }
            curl_slist_free_all(*plist);
        }
        return out;
    }

    void load_cookie(std::filesystem::path p) {
        CurlEasy x;
        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        // append filename
        x.setopt(CURLoption::CURLOPT_COOKIEFILE, p.c_str());
        // actually load
        x.setopt(CURLoption::CURLOPT_COOKIELIST, "RELOAD");
    }

    void save_cookie(std::filesystem::path p) const {
        CurlEasy x;
        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        x.setopt(CURLoption::CURLOPT_COOKIEJAR, p.c_str());
        // save when x destruct
    }

private:
    std::error_code apply_multi_options() {
        if (m_options.max_idle_connections > 0) {
            if (auto ec = curl_multi_setopt(
                    m_multi, CURLMoption::CURLMOPT_MAXCONNECTS, m_options.max_idle_connections)) {
                return ec;
            }
        }
        if (m_options.max_host_connections > 0) {
            if (auto ec = curl_multi_setopt(m_multi,
                                            CURLMoption::CURLMOPT_MAX_HOST_CONNECTIONS,
                                            m_options.max_host_connections)) {
                return ec;
            }
        }
        if (m_options.max_total_connections > 0) {
            if (auto ec = curl_multi_setopt(m_multi,
                                            CURLMoption::CURLMOPT_MAX_TOTAL_CONNECTIONS,
                                            m_options.max_total_connections)) {
                return ec;
            }
        }
        if (m_options.enable_http_multiplex) {
            if (auto ec = curl_multi_setopt(m_multi, CURLMoption::CURLMOPT_PIPELINING, 2L)) {
                return ec;
            }
        }
        return {};
    }

    std::error_code apply_easy_options(CurlEasy& easy) {
        if (m_options.receive_buffer_size > 0) {
            if (auto ec = easy.setopt(CURLoption::CURLOPT_BUFFERSIZE,
                                      m_options.receive_buffer_size)) {
                return ec;
            }
        }
        if (m_options.upload_buffer_size > 0) {
            if (auto ec = easy.setopt(CURLoption::CURLOPT_UPLOAD_BUFFERSIZE,
                                      m_options.upload_buffer_size)) {
                return ec;
            }
        }
        return {};
    }

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
    CurlOptions m_options;

    std::mutex m_share_mutex;
};
} // namespace ncrequest
