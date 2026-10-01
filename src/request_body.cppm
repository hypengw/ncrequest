export module ncrequest:request_body;
export import :error;
export import :client_callback;
export import rstd;

using namespace rstd::prelude;
using rstd::bytes::Bytes;

namespace ncrequest
{

export struct BodyReader {
    using Callback = client::Callback<usize(byte*, usize)>;
    Callback      callback;
    Option<usize> size;
};

export class RequestBody {
    Option<Bytes>      bytes_;
    Option<BodyReader> reader_;

public:
    static auto from_bytes(Bytes bytes) -> RequestBody {
        auto body   = RequestBody {};
        body.bytes_ = Some(rstd::move(bytes));
        return body;
    }
    static auto from_reader(BodyReader reader) -> Result<RequestBody> {
        if (! reader.callback) return Err(Error::InvalidState("body reader callback is empty"));
        auto body    = RequestBody {};
        body.reader_ = Some(rstd::move(reader));
        return Ok(rstd::move(body));
    }
    auto bytes() const -> const Option<Bytes>& { return bytes_; }
    auto reader() const -> const Option<BodyReader>& { return reader_; }
    auto try_clone() const -> Result<RequestBody> {
        if (reader_.is_some()) return Err(Error::Unsupported("body reader cannot be cloned"));
        if (bytes_.is_some()) return Ok(from_bytes(Bytes::copy_from_slice(bytes_->as_slice())));
        return Ok(RequestBody {});
    }
};

} // namespace ncrequest
