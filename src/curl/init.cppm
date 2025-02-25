module;
#include <system_error>
#include <memory_resource>

export module ncrequest.curl:init;

namespace ncrequest
{
export auto curl_init(std::pmr::memory_resource* resource = nullptr) -> std::error_code;
} // namespace ncrequest