module;
export module ncrequest.curl:easy;
export import rstd;
export import :curl;

using namespace rstd::prelude;
using namespace curl;
using namespace rstd::literals;
using rstd::ffi::CString;

namespace ncrequest
{
template<CURLoption OPT>
struct curl_opt_traits;
template<>
struct curl_opt_traits<CURLoption::CURLOPT_SHARE> {
    using type = CURLSH*;
};

export class CurlEasy {
public:
    CurlEasy(const CurlEasy&)                    = delete;
    auto operator=(const CurlEasy&) -> CurlEasy& = delete;

    CurlEasy() noexcept: easy(curl_easy_init()), m_headers(nullptr), m_share(nullptr) {
        if (easy == nullptr) {
            m_error = CURLcode::CURLE_OUT_OF_MEMORY;
            return;
        }
        // enable cookie engine
        setopt<CURLoption::CURLOPT_COOKIEFILE>("");

        // thread safe
        setopt<CURLoption::CURLOPT_NOSIGNAL>(1L);

        setopt<CURLoption::CURLOPT_SUPPRESS_CONNECT_HEADERS>(1L);
        setopt<CURLoption::CURLOPT_FOLLOWLOCATION>(0L);
        setopt<CURLoption::CURLOPT_AUTOREFERER>(1L);
        setopt<CURLoption::CURLOPT_VERBOSE>(0L);
    }

    ~CurlEasy() {
        curl_easy_cleanup(easy);
        curl_slist_free_all(m_headers);
    }

    auto status() const noexcept -> CURLcode { return m_error; }

    CURL* handle() const noexcept { return easy; }

    template<typename T>
    auto curl_private() {
        return get_info<T>(CURLINFO::CURLINFO_PRIVATE);
    }

    template<typename T>
    inline auto get_info(CURLINFO info) noexcept -> rstd::Result<T, CURLcode> {
        if (m_error != CURLcode::CURLE_OK) return Err(m_error);
        T inst {};
        if (auto res = curl_easy_getinfo(handle(), info, &inst)) {
            return Err(res);
        }
        return Ok(rstd::move(inst));
    }

    template<CURLoption OPT>
    auto getopt() noexcept -> typename curl_opt_traits<OPT>::type {
        static_assert(false);
    }

    template<CURLoption OPT, typename T>
    constexpr auto setopt(T para) noexcept -> CURLcode {
        return setopt(OPT, para);
    }

    template<typename T>
    auto setopt(CURLoption opt, T para) noexcept -> CURLcode {
        if (m_error == CURLcode::CURLE_OK) m_error = curl_easy_setopt(handle(), opt, para);
        return m_error;
    }

    CURLcode perform() noexcept {
        return m_error == CURLcode::CURLE_OK ? curl_easy_perform(easy) : m_error;
    }

    template<typename Headers>
    auto set_header(const Headers& headers) -> CURLcode {
        if (reset_header() != CURLcode::CURLE_OK) return m_error;
        for (const auto& field : headers) {
            auto name  = field.name.as_str();
            auto value = field.value.as_slice();

            auto bytes = Vec<u8>::with_capacity(name.size() + value.len() + usize(2));
            bytes.extend_from_slice(name.as_bytes());
            bytes.extend_from_slice(": "_bytes);
            bytes.extend_from_slice(value);
            auto  header   = CString::from_vec_unchecked(rstd::move(bytes));
            auto* appended = curl_slist_append(m_headers, header.as_ptr());
            if (appended == nullptr) {
                m_error = CURLcode::CURLE_OUT_OF_MEMORY;
                return m_error;
            }
            m_headers = appended;
        }
        return setopt<CURLoption::CURLOPT_HTTPHEADER>(m_headers);
    }

    auto reset_header() -> CURLcode {
        if (setopt<CURLoption::CURLOPT_HTTPHEADER>(static_cast<curl_slist*>(nullptr)) !=
            CURLcode::CURLE_OK)
            return m_error;
        curl_slist_free_all(m_headers);
        m_headers = nullptr;
        return m_error;
    }

    CURLcode pause(int bitmask) noexcept { return curl_easy_pause(handle(), bitmask); }

private:
    CURLcode    m_error { CURLcode::CURLE_OK };
    CURL*       easy;
    curl_slist* m_headers;
    CURLSH*     m_share;
};

template<>
inline auto CurlEasy::setopt<CURLoption::CURLOPT_SHARE, CURLSH*>(CURLSH* para) noexcept
    -> CURLcode {
    auto code = setopt(CURLoption::CURLOPT_SHARE, para);
    if (code == CURLcode::CURLE_OK) m_share = para;
    return code;
}

template<>
inline auto CurlEasy::getopt<CURLoption::CURLOPT_SHARE>() noexcept -> CURLSH* {
    return m_share;
}
} // namespace ncrequest
