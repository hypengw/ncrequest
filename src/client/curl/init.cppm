export module ncrequest.curl:init;
export import rstd.core;
export import cppstd;

namespace ncrequest
{
export auto curl_init(std::pmr::memory_resource* resource = nullptr) -> std::error_code;
} // namespace ncrequest