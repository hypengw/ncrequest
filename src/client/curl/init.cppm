export module ncrequest.curl:init;
export import :curl;
export import rstd.core;
export import cppstd;

using namespace rstd::prelude;
using std::pmr::memory_resource;

namespace ncrequest
{
export auto curl_init(memory_resource* resource = nullptr) -> rstd::Result<empty, curl::CURLcode>;
} // namespace ncrequest
