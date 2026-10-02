export module ncrequest:request_body;
export import :error;
export import :client_callback;
export import ncrequest.coro;
export import lihttpto;
export import rstd;

using namespace rstd::prelude;
using rstd::bytes::Bytes;
using rstd::mtp::same_as;
using rstd::sync::Arc;

namespace ncrequest
{

export struct BodyReader {
    using Callback = client::Callback<usize(byte*, usize)>;
    Callback      callback;
    Option<usize> size;
};

export struct BodyStream {
    using Callback = client::Callback<coro<Result<Option<Bytes>>>()>;
    Callback    next;
    Option<u64> size;
};

export class RequestBody {
    Option<Bytes>      bytes_;
    Option<BodyReader> reader_;
    Option<BodyStream> stream_;

    template<lihttpto::BodySource Source>
    static auto source_next(Arc<Source> source) -> coro<Result<Option<Bytes>>> {
        auto result = co_await source->next();
        if (result.is_err()) {
            if constexpr (same_as<typename Source::Error, Error>)
                co_return Err(rstd::move(result).unwrap_err());
            else
                co_return Err(rstd::into<Error>(rstd::move(result).unwrap_err()));
        }
        co_return Ok(rstd::move(result).unwrap());
    }

public:
    // A known size stops polling at that byte count; unknown size reads through EOF.
    template<lihttpto::BodySource Source>
        requires(same_as<typename Source::Error, Error> ||
                 requires(typename Source::Error error) { rstd::into<Error>(rstd::move(error)); })
    static auto from_source(Source source, Option<u64> size = None<u64>()) -> RequestBody {
        auto owned   = Arc<Source>::make(rstd::move(source));
        auto body    = RequestBody {};
        body.stream_ = Some(BodyStream { [owned = rstd::move(owned)]() {
                                            return source_next(owned.clone());
                                        },
                                         size });
        return body;
    }
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
    auto stream() const -> const Option<BodyStream>& { return stream_; }
    auto try_clone() const -> Result<RequestBody> {
        if (stream_.is_some()) return Err(Error::Unsupported("body source cannot be cloned"));
        if (reader_.is_some()) return Err(Error::Unsupported("body reader cannot be cloned"));
        if (bytes_.is_some()) return Ok(from_bytes(Bytes::copy_from_slice(bytes_->as_slice())));
        return Ok(RequestBody {});
    }
};

} // namespace ncrequest
