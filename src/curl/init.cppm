module;
#include <system_error>
#include <memory_resource>

export module ncrequest.curl:init;

namespace ncrequest
{
export auto curl_init() -> std::error_code;
export auto curl_init(std::pmr::memory_resource* resource) -> std::error_code;
} // namespace ncrequest