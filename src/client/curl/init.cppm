export module ncrequest.curl:init;
export import :curl;
export import rstd.core;
export import cppstd;

namespace ncrequest
{
export auto curl_init(std::pmr::memory_resource* resource = nullptr)
    -> rstd::Result<rstd::empty, curl::CURLcode>;
} // namespace ncrequest
