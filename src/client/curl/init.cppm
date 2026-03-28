export module ncrequest.curl:init;
export import rstd.core;
export import cppstd;

namespace ncrequest
{
export auto curl_init(cppstd::pmr::memory_resource* resource = nullptr) -> cppstd::error_code;
} // namespace ncrequest