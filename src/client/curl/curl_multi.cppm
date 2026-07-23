module;
#include <rstd/enum.hpp>

export module ncrequest.curl:multi;
export import :easy;
export import ncrequest.type;

using namespace curl;

namespace ncrequest
{

using rstd::path::Path;

export struct CurlOptions {
    long max_idle_connections { 0 };
    long max_host_connections { 0 };
    long max_total_connections { 0 };
    long receive_buffer_size { 0 };
    long upload_buffer_size { 0 };
    bool enable_http_multiplex { false };
};

export struct CurlMultiError {
    RSTD_ENUM(CurlMultiError, (Easy, (CURLcode code;)), (Multi, (CURLMcode code;)))
};

export using CurlMultiResult = rstd::Result<empty, CurlMultiError>;

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
          m_options(options),
          m_share_mutex(empty {}) {
        // curl_multi_setopt(m_multi, CURLMOPT_SOCKETFUNCTION, CurlMulti::curl_socket_func);
        // curl_multi_setopt(m_multi, CURLMOPT_SOCKETDATA, this);
        // curl_multi_setopt(m_multi, CURLMOPT_TIMERFUNCTION, CurlMulti::curl_timer_func);
        // curl_multi_setopt(m_multi, CURLMOPT_TIMERDATA, this);

        curl_share_setopt(
            m_share, CURLSHoption::CURLSHOPT_SHARE, curl_lock_data::CURL_LOCK_DATA_COOKIE);
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

    auto add_handle(CurlEasy& easy) -> CurlMultiResult {
        auto applied = apply_easy_options(easy);
        if (applied.is_err()) return applied;
        if (easy.getopt<CURLoption::CURLOPT_SHARE>() == nullptr) {
            auto code = easy.setopt<CURLoption::CURLOPT_SHARE>(m_share);
            if (code != CURLcode::CURLE_OK) {
                return rstd::Err(CurlMultiError::Easy(code));
            }
        }
        auto code = curl_multi_add_handle(m_multi, easy.handle());
        if (code != CURLMcode::CURLM_OK) {
            return rstd::Err(CurlMultiError::Multi(code));
        }
        return rstd::Ok(empty {});
    }

    auto set_options(CurlOptions options) -> CurlMultiResult {
        auto old     = m_options;
        m_options    = options;
        auto applied = apply_multi_options();
        if (applied.is_err()) {
            m_options = old;
            return applied;
        }
        return rstd::Ok(empty {});
    }

    auto options() const noexcept -> const CurlOptions& { return m_options; }

    auto remove_handle(CurlEasy& easy) -> CurlMultiResult {
        return multi_result(curl_multi_remove_handle(m_multi, easy.handle()));
    }
    auto remove_handle(CURL* easy) -> CurlMultiResult {
        return multi_result(curl_multi_remove_handle(m_multi, easy));
    }

    auto wakeup() -> CurlMultiResult { return multi_result(curl_multi_wakeup(m_multi)); }

    auto perform(int& still_running) -> CurlMultiResult {
        return multi_result(curl_multi_perform(m_multi, &still_running));
    }

    auto poll(rstd::time::Duration timeout) -> CurlMultiResult {
        return multi_result(curl_multi_poll(
            m_multi, nullptr, 0, static_cast<int>(timeout.as_millis().to_primitive()), nullptr));
    }

    auto query_info_msg() -> rstd::vec::Vec<InfoMsg> {
        auto out = rstd::vec::Vec<InfoMsg>::make();
        int  message_left { 0 };
        while (CURLMsg* msg = curl_multi_info_read(m_multi, &message_left)) {
            out.push(InfoMsg {
                .msg         = msg->msg,
                .easy_handle = msg->easy_handle,
                .result      = msg->data.result,
            });
        }
        return out;
    }

    auto cookies() const -> rstd::vec::Vec<rstd::string::String> {
        auto     out = rstd::vec::Vec<rstd::string::String>::make();
        CurlEasy x;

        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        auto list_ = x.get_info<curl_slist*>(CURLINFO::CURLINFO_COOKIELIST);
        if (list_.is_ok()) {
            auto list = rstd::move(list_).unwrap();
            auto head = list;
            while (list) {
                auto text = rstd::ffi::CStr::from_ptr(list->data).to_str();
                if (text.is_ok()) {
                    out.push(rstd::string::String::make(rstd::move(text).unwrap()));
                }
                list = list->next;
            }
            curl_slist_free_all(head);
        }
        return out;
    }

    void load_cookie(ref<Path> path) {
        auto filename = path.to_cstring();
        if (filename.is_err()) return;
        auto owned_filename = rstd::move(filename).unwrap();

        CurlEasy x;
        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        // append filename
        x.setopt(CURLoption::CURLOPT_COOKIEFILE,
                 owned_filename.as_ptr());
        // actually load
        x.setopt(CURLoption::CURLOPT_COOKIELIST, "RELOAD");
    }

    void save_cookie(ref<Path> path) const {
        auto filename = path.to_cstring();
        if (filename.is_err()) return;
        auto owned_filename = rstd::move(filename).unwrap();

        CurlEasy x;
        x.setopt(CURLoption::CURLOPT_SHARE, m_share);
        x.setopt(CURLoption::CURLOPT_COOKIEJAR,
                 owned_filename.as_ptr());
        // save when x destruct
    }

private:
    auto apply_multi_options() -> CurlMultiResult {
        if (m_options.max_idle_connections > 0) {
            if (auto ec = curl_multi_setopt(
                    m_multi, CURLMoption::CURLMOPT_MAXCONNECTS, m_options.max_idle_connections)) {
                return rstd::Err(CurlMultiError::Multi(ec));
            }
        }
        if (m_options.max_host_connections > 0) {
            if (auto ec = curl_multi_setopt(m_multi,
                                            CURLMoption::CURLMOPT_MAX_HOST_CONNECTIONS,
                                            m_options.max_host_connections)) {
                return rstd::Err(CurlMultiError::Multi(ec));
            }
        }
        if (m_options.max_total_connections > 0) {
            if (auto ec = curl_multi_setopt(m_multi,
                                            CURLMoption::CURLMOPT_MAX_TOTAL_CONNECTIONS,
                                            m_options.max_total_connections)) {
                return rstd::Err(CurlMultiError::Multi(ec));
            }
        }
        if (m_options.enable_http_multiplex) {
            if (auto ec = curl_multi_setopt(m_multi, CURLMoption::CURLMOPT_PIPELINING, 2L)) {
                return rstd::Err(CurlMultiError::Multi(ec));
            }
        }
        return rstd::Ok(empty {});
    }

    auto apply_easy_options(CurlEasy& easy) -> CurlMultiResult {
        if (m_options.receive_buffer_size > 0) {
            if (auto ec =
                    easy.setopt(CURLoption::CURLOPT_BUFFERSIZE, m_options.receive_buffer_size)) {
                return rstd::Err(CurlMultiError::Easy(ec));
            }
        }
        if (m_options.upload_buffer_size > 0) {
            if (auto ec = easy.setopt(CURLoption::CURLOPT_UPLOAD_BUFFERSIZE,
                                      m_options.upload_buffer_size)) {
                return rstd::Err(CurlMultiError::Easy(ec));
            }
        }
        return rstd::Ok(empty {});
    }

    static auto multi_result(CURLMcode code) -> CurlMultiResult {
        if (code == CURLMcode::CURLM_OK) return rstd::Ok(empty {});
        return rstd::Err(CurlMultiError::Multi(code));
    }

    static void static_share_lock(CURL*, curl_lock_data data, curl_lock_access, void* clientp) {
        auto info = static_cast<CurlMulti*>(clientp);
        if (data == curl_lock_data::CURL_LOCK_DATA_COOKIE) {
            info->m_share_guard = rstd::Some(info->m_share_mutex.lock().unwrap());
        }
    }

    static void static_share_unlock(CURL*, curl_lock_data data, void* clientp) {
        auto info = static_cast<CurlMulti*>(clientp);
        if (data == curl_lock_data::CURL_LOCK_DATA_COOKIE) {
            (void)info->m_share_guard.take();
        }
    }

private:
    CURLM*      m_multi;
    CURLSH*     m_share;
    CurlOptions m_options;

    rstd::sync::Mutex<empty>                    m_share_mutex;
    rstd::Option<rstd::sync::MutexGuard<empty>> m_share_guard;
};
} // namespace ncrequest
