export module ncrequest.curl:init;
export import rstd.core;

namespace ncrequest
{
export auto curl_init(cppstd::pmr::memory_resource* resource = nullptr) -> cppstd::error_code;
} // namespace ncrequest