export module ncrequest:cache;
export import :response;
export import :request_control;
export import :cache_mode;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
export namespace ncrequest
{
auto cache_now() -> i64 {
    auto value = rstd::time::SystemTime::now().duration_since(rstd::time::SystemTime::unix_epoch());
    return value.is_ok() ? i64(value->as_secs().to_primitive()) : i64();
}
struct CachePolicy {
    lihttpto::CacheFields fields;
    i64                   request_time {}, response_time {}, date_anchor {};
    static auto           make(const lihttpto::Headers& headers, i64 requested, i64 received,
                               Option<i64> anchor = None()) -> CachePolicy {
        return { lihttpto::CacheFields::parse(headers),
                 requested,
                 received,
                 anchor.is_some() ? *anchor : received };
    }
    auto age(i64 now) const -> i64 {
        auto apparent = rstd::cmp::max(
            i64(), response_time - (fields.date.is_some() ? *fields.date : date_anchor));
        auto initial = rstd::cmp::max(
            apparent, fields.age + rstd::cmp::max(i64(), response_time - request_time));
        return initial + rstd::cmp::max(i64(), now - response_time);
    }
    auto lifetime() const -> i64 {
        if (fields.control.max_age.is_some()) return *fields.control.max_age;
        return fields.expires.is_some()
                   ? rstd::cmp::max(i64(),
                                    *fields.expires -
                                        (fields.date.is_some() ? *fields.date : date_anchor))
                   : i64();
    }
    auto fresh(i64 now, const lihttpto::CacheControl& request) const -> bool {
        if (! fields.reusable || fields.control.no_cache || request.no_cache || request.invalid)
            return false;
        auto limit = lifetime();
        if (request.max_age.is_some()) limit = rstd::min(limit, *request.max_age);
        return age(now) + (request.min_fresh.is_some() ? *request.min_fresh : i64()) < limit;
    }
    template<class Equal>
    auto matches_request(Equal equal) const -> bool {
        if (! fields.reusable || ! equal("authorization"_str)) return false;
        for (const auto& name : fields.vary)
            if (! equal(name.as_str())) return false;
        return true;
    }
    template<class Field>
    void request_variant(Field field) const {
        field("authorization"_str);
        for (const auto& name : fields.vary) field(name.as_str());
    }
    auto browser_headers(const lihttpto::Headers& source, i64 now) const -> lihttpto::Headers {
        lihttpto::Headers out;
        // Preserve only representation and caching fields, never transport or
        // cookie state.
        for (auto name : array<ref<str>, 7> { "content-type"_str,
                                              "content-encoding"_str,
                                              "etag"_str,
                                              "last-modified"_str,
                                              "date"_str,
                                              "expires"_str,
                                              "vary"_str })
            for (auto value : source.get_all(name))
                out.push(lihttpto::Header::make(name, value->as_slice()).unwrap());
        auto directive =
            fields.control.no_store ? String::make("private, no-store"_str)
            : fields.control.no_cache || fields.control.invalid || ! fields.reusable
                ? String::make("private, no-cache"_str)
                : rstd::format("private, max-age={}{}",
                               lifetime(),
                               fields.control.must_revalidate ? ", must-revalidate"_str : ""_str);
        out.push(
            lihttpto::Header::make("cache-control"_str, directive.as_str().as_bytes()).unwrap());
        auto current = rstd::format("{}", age(now));
        out.push(lihttpto::Header::make("age"_str, current.as_str().as_bytes()).unwrap());
        return out;
    }
};
struct CacheMetadata {
    lihttpto::ResponseHead head;
    CachePolicy            policy;
    auto                   clone() const -> CacheMetadata {
        auto copy = CachePolicy {
            policy.fields.clone(), policy.request_time, policy.response_time, policy.date_anchor
        };
        return { { head.status, head.headers.clone(), head.version, head.reason.clone() },
                 rstd::move(copy) };
    }
};
struct CacheQuery {
    String            scope, url;
    lihttpto::Headers headers;
};
struct CacheRecord {
    CacheMetadata  metadata;
    ResponseSource body;
};
struct CacheWrite {
    template<class Self, class Delegate = void>
    struct Api {
        using Trait = CacheWrite;
        auto write(Bytes bytes) -> coro<Result<empty>> {
            return rstd::trait_call<0>(this, rstd::move(bytes));
        }
        auto publish(CacheMetadata metadata, bool reuse) -> coro<Result<empty>> {
            return rstd::trait_call<1>(this, rstd::move(metadata), reuse);
        }
        void abort() { rstd::trait_call<2>(this); }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::write, &T::publish, &T::abort>;
};
using CacheWriter = Arc<dyn<CacheWrite>>;
struct CacheAccess {
    Option<CacheRecord> record;
    Option<CacheWriter> writer;
};
struct CacheStorage {
    template<class Self, class Delegate = void>
    struct Api {
        using Trait = CacheStorage;
        auto open(CacheQuery query, Arc<RequestControl> control) -> coro<Result<CacheAccess>> {
            return rstd::trait_call<0>(this, rstd::move(query), rstd::move(control));
        }
        void report(ErrorKind kind) { rstd::trait_call<1>(this, kind); }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::open, &T::report>;
};
struct CacheConfiguration {
    Arc<dyn<CacheStorage>> storage;
    String                 scope;
};

struct CachedReader {
    ResponseSource                        source;
    Arc<RequestControl>                   control;
    Arc<rstd::sync::atomic::Atomic<bool>> closed;
    auto                                  next_chunk() -> coro<Result<Option<Bytes>>> {
        if (control->is_canceled() || closed->load()) {
            source->cancel();
            co_return Err(Error::Canceled());
        }
        co_return co_await source->next_chunk();
    }
    auto trailers() const { return source->trailers(); }
    auto is_finished() const -> bool { return source->is_finished(); }
    void cancel() { source->cancel(); }
};
struct WritingReader {
    ResponseBody           body;
    Arc<Response>          response;
    Option<CacheWriter>    writer;
    Arc<dyn<CacheStorage>> storage;
    CacheMetadata          metadata;
    u64                    received {};
    bool                   finished {};
    ~WritingReader() { cancel(); }
    WritingReader(ResponseBody body, Arc<Response> response, CacheWriter writer,
                  Arc<dyn<CacheStorage>> storage, CacheMetadata metadata)
        : body(rstd::move(body)),
          response(rstd::move(response)),
          writer(Some(rstd::move(writer))),
          storage(rstd::move(storage)),
          metadata(rstd::move(metadata)) {}
    WritingReader(WritingReader&& other)
        : body(rstd::move(other.body)),
          response(rstd::move(other.response)),
          writer(other.writer.take()),
          storage(rstd::move(other.storage)),
          metadata(rstd::move(other.metadata)),
          received(other.received),
          finished(other.finished) {
        other.finished = true;
    }
    auto next_chunk() -> coro<Result<Option<Bytes>>> {
        auto part = co_await body.next();
        if (part.is_err()) {
            cancel();
            co_return Err(rstd::move(part).unwrap_err());
        }
        if (writer.is_some()) {
            Result<empty> stored = Ok(empty {});
            if (part->is_some()) {
                received += u64((**part).size().to_primitive());
                stored = co_await (*writer)->write(Bytes::copy_from_slice((**part).as_slice()));
            } else {
                auto length   = lihttpto::header_text(metadata.head.headers, "content-length"_str);
                auto declared = length.is_some() ? lihttpto::delta_seconds(*length) : None<i64>();
                if (length.is_some() &&
                    (declared.is_none() || u64(declared->to_primitive()) != received)) {
                    (*writer)->abort();
                    writer = None();
                    co_return Err(Error::Protocol(ProtocolError::UnexpectedEof,
                                                  "cached response length mismatch"));
                }
                stored = co_await (*writer)->publish(metadata.clone(), false);
            }
            if (stored.is_err()) {
                storage->report(stored.unwrap_err().kind());
                (*writer)->abort();
                writer = None();
            } else if (part->is_none())
                writer = None();
        }
        if (part->is_none()) finished = true;
        co_return rstd::move(part);
    }
    auto trailers() const { return response->trailers(); }
    auto is_finished() const -> bool { return finished; }
    void cancel() {
        if (writer.is_some()) {
            (*writer)->abort();
            writer = None();
        }
        if (! finished) body.cancel();
    }
};
} // namespace ncrequest
namespace rstd
{
export template<class T>
    requires requires(T& value, ncrequest::CacheMetadata metadata, bytes::Bytes bytes) {
        {
            value.write(rstd::move(bytes))
        } -> mtp::same_as<ncrequest::coro<ncrequest::Result<empty>>>;
        {
            value.publish(rstd::move(metadata), false)
        } -> mtp::same_as<ncrequest::coro<ncrequest::Result<empty>>>;
        value.abort();
    }
struct Impl<ncrequest::CacheWrite, T> : ImplBase<T> {
    auto write(bytes::Bytes bytes) { return this->self().write(rstd::move(bytes)); }
    auto publish(ncrequest::CacheMetadata metadata, bool reuse) {
        return this->self().publish(rstd::move(metadata), reuse);
    }
    void abort() { this->self().abort(); }
};
export template<class T>
    requires requires(T& value, ncrequest::CacheQuery query,
                      sync::Arc<ncrequest::RequestControl> control) {
        {
            value.open(rstd::move(query), rstd::move(control))
        } -> mtp::same_as<ncrequest::coro<ncrequest::Result<ncrequest::CacheAccess>>>;
        value.report(ncrequest::ErrorKind::Io);
    }
struct Impl<ncrequest::CacheStorage, T> : ImplBase<T> {
    auto open(ncrequest::CacheQuery query, sync::Arc<ncrequest::RequestControl> control) {
        return this->self().open(rstd::move(query), rstd::move(control));
    }
    void report(ncrequest::ErrorKind kind) { this->self().report(kind); }
};
} // namespace rstd
