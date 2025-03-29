module;
export module ncrequest:error;
export import rstd;

namespace ncrequest
{

struct Error {};

export template<typename T>
using Result = rstd::Result<T, Error>;

} // namespace ncrequest
