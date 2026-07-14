module;
export module ncrequest.curl:easy;
export import ncrequest.type;
export import cppstd;
export import :curl;

using namespace curl;

namespace ncrequest
{
namespace detail
{
template<CURLoption OPT>
struct curl_opt_traits;
template<>
struct curl_opt_traits<CURLoption::CURLOPT_SHARE> {
    using type = CURLSH*;
};
} // namespace detail

export class CurlEasy : NoCopy {
public:
    CurlEasy() noexcept: easy(curl_easy_init()), m_headers(nullptr), m_share(nullptr) {
        // enable cookie engine
        setopt<CURLoption::CURLOPT_COOKIEFILE>("");

        // thread safe
        setopt<CURLoption::CURLOPT_NOSIGNAL>(1L);

        setopt<CURLoption::CURLOPT_FOLLOWLOCATION>(1L);
        setopt<CURLoption::CURLOPT_AUTOREFERER>(1L);
        setopt<CURLoption::CURLOPT_VERBOSE>(0L);
    }

    ~CurlEasy() {
        reset_header();
        curl_easy_cleanup(easy);
    }

    CURL* handle() const noexcept { return easy; }

    template<typename T>
    auto curl_private() {
        return get_info<T>(CURLINFO::CURLINFO_PRIVATE);
    }

    template<typename T>
    inline auto get_info(CURLINFO info) noexcept -> rstd::Result<T, CURLcode> {
        T inst;
        if (auto res = curl_easy_getinfo(handle(), info, &inst)) {
            return rstd::Err(res);
        }
        return rstd::Ok(rstd::move(inst));
    }

    template<CURLoption OPT>
    auto getopt() noexcept -> typename detail::curl_opt_traits<OPT>::type {
        static_assert(false);
    }

    template<CURLoption OPT, typename T>
    constexpr auto setopt(T para) noexcept -> CURLcode {
        return curl_easy_setopt(handle(), OPT, para);
    }

    template<typename T>
    auto setopt(CURLoption opt, T para) noexcept -> CURLcode {
        return curl_easy_setopt(handle(), opt, para);
    }

    CURLcode perform() noexcept { return curl_easy_perform(easy); }

    template<typename Headers>
    void set_header(const Headers& headers) {
        reset_header();
        auto fields = headers.iter();
        for (auto field = fields.next(); field.is_some(); field = fields.next()) {
            auto name  = (**field).name().as_ref();
            auto value = (**field).value().as_bytes();

            std::string header;
            header.reserve(name.size() + value.len() + 2);
            header.append(reinterpret_cast<const char*>(name.data()), name.size());
            header.append(": ");
            header.append(reinterpret_cast<const char*>(value.as_raw_ptr()), value.len());
            m_headers = curl_slist_append(m_headers, header.c_str());
        }
        if (m_headers != nullptr) setopt<CURLoption::CURLOPT_HTTPHEADER>(m_headers);
    }

    void reset_header() {
        setopt<CURLoption::CURLOPT_HTTPHEADER>(nullptr);
        curl_slist_free_all(m_headers);
        m_headers = nullptr;
    }

    CURLcode pause(int bitmask) noexcept { return curl_easy_pause(handle(), bitmask); }

private:
    CURL*       easy;
    curl_slist* m_headers;
    CURLSH*     m_share;
};

template<>
inline auto CurlEasy::setopt<CURLoption::CURLOPT_SHARE, CURLSH*>(CURLSH* para) noexcept
    -> CURLcode {
    auto code = curl_easy_setopt(handle(), CURLoption::CURLOPT_SHARE, para);
    m_share   = para;
    return code;
}

template<>
inline auto CurlEasy::getopt<CURLoption::CURLOPT_SHARE>() noexcept -> CURLSH* {
    return m_share;
}
} // namespace ncrequest
