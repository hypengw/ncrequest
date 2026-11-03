module ncrequest:client.io;
import :session;
import :client.callback;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client
{
namespace lf = lifetchto;
namespace h  = lifetchto::http;
using lf::Arc;
using lf::Bytes;
using lf::coro;
struct ExchangeFields {
    Option<lihttpto::ResponseHead> head;
    h::CacheObservation            cache;
    lihttpto::Headers              trailers;
    Option<lf::Error>              error;
    Option<lf::FailureReport>      failure;
    bool                           failed {}, published {};
    bool                           demand {}, done {}, dispatched {}, stopped {};
    bool                           trailers_available { true };
};
struct Exchange {
    h::PreparedExchange               prepared;
    rstd::sync::Mutex<h::UploadBody>  upload;
    rstd::sync::Mutex<ExchangeFields> fields { ExchangeFields {} };
    Option<lf::ByteQueue>             outgoing, incoming;
    rstd::sync::atomic::Atomic<bool>  ready { false };
    Option<lf::TaskControl>           producer, progress;
    Option<lf::ResourcePermit>        native_permit;
    lf::Cancellation                  upload_stop;
    lf::Event                         changed;
    mutable Callback<void()>          wake;
    usize                             window, native_bytes;
    Exchange(h::ExchangeInput input, usize window, usize head_limit)
        : prepared(rstd::move(input.prepared)),
          upload(rstd::move(input.body)),
          window(window),
          native_bytes(head_limit + window * usize(2)) {}
    void notify() const {
        changed.notify();
        if (wake) wake();
    }
    void stop_upload() const {
        upload_stop.cancel();
        upload.lock().unwrap()->cancel();
        notify();
    }
    void fail(lf::Error error) const {
        bool published;
        {
            auto f = fields.lock().unwrap();
            if (f->failed) return;
            f->failed  = true;
            f->failure = Some(lf::report_failure(error));
            published  = f->published;
            if (! published) f->error = Some(rstd::move(error));
        }
        if (published) incoming->fail(rstd::move(error));
        stop_upload();
        notify();
    }
    void stop(lf::StopReason reason = lf::StopReason::Canceled) const {
        {
            auto f = fields.lock().unwrap();
            if (f->done) return;
            f->stopped = true;
        }
        fail(lf::stop_error(reason));
    }
    void complete() const {
        {
            auto f  = fields.lock().unwrap();
            f->done = true;
        }
        if (incoming.is_some()) incoming->finish();
        stop_upload();
        notify();
    }
    void demand() const {
        fields.lock().unwrap()->demand = true;
        notify();
    }
};
struct UploadTask {
    Arc<Exchange> exchange;
    auto          run(lf::Cancellation) const -> coro<lf::Result<empty>> {
        auto                  stop = exchange->upload_stop.clone();
        lihttpto::BodyEncoder validator(
            { lihttpto::BodyKind::UntilEof }, exchange->window, exchange->prepared.upload().length);
        for (;;) {
            auto event = exchange->changed.listen();
            if (stop.is_canceled()) co_return Ok(empty {});
            bool demand;
            {
                auto f = exchange->fields.lock().unwrap();
                demand = rstd::exchange(f->demand, false);
            }
            if (! demand) {
                (void)co_await rstd::async::select(rstd::move(event), stop.stopped().into_future());
                continue;
            }
            if (validator.remaining() == Some(u64())) {
                exchange->outgoing->finish();
                exchange->notify();
                co_return Ok(empty {});
            }
            auto reading = [&] {
                auto body = exchange->upload.lock().unwrap();
                return body->next(exchange->window);
            }();
            auto ready = co_await rstd::async::select(rstd::move(reading).into_future(),
                                                      stop.stopped().into_future());
            if (ready.is_right()) co_return Ok(empty {});
            auto part = rstd::move(ready).unwrap_left();
            if (part.is_err()) {
                exchange->fail(rstd::move(part).unwrap_err());
                co_return Ok(empty {});
            }
            if (part->is_none()) {
                if (validator.finish().is_err()) {
                    exchange->fail(h::protocol_error("upload ended before declared length"_Str));
                    co_return Ok(empty {});
                }
                exchange->outgoing->finish();
                exchange->notify();
                co_return Ok(empty {});
            }
            auto checked = validator.encode(lihttpto::BodyFrame::Data(part->take().unwrap()));
            if (checked.is_err()) {
                exchange->fail(h::protocol_error("upload exceeds declared length"_Str));
                co_return Ok(empty {});
            }
            auto sent = co_await exchange->outgoing->send(rstd::move(checked).unwrap());
            exchange->notify();
            if (sent.is_err()) {
                if (! stop.is_canceled()) exchange->fail(rstd::move(sent).unwrap_err());
                co_return Ok(empty {});
            }
            if (validator.remaining() == Some(u64())) {
                exchange->outgoing->finish();
                exchange->notify();
                co_return Ok(empty {});
            }
        }
    }
    void request_stop(lf::StopReason) const { exchange->stop_upload(); }
};
auto begin_exchange(Arc<Exchange> exchange, lf::TransferContext context)
    -> coro<lf::Result<empty>> {
    auto attempt = context.begin_attempt();
    if (attempt.is_err()) co_return Err(rstd::move(attempt).unwrap_err());
    auto permit = co_await context.resources().reserve(
        lf::ResourceAmount { .resources = usize(1), .bytes = exchange->native_bytes });
    if (permit.is_err()) co_return Err(rstd::move(permit).unwrap_err());
    exchange->native_permit = Some(rstd::move(permit).unwrap());
    auto incoming           = co_await lf::ByteQueue::make(context.resources(), exchange->window);
    if (incoming.is_err()) co_return Err(rstd::move(incoming).unwrap_err());
    exchange->incoming = Some(rstd::move(incoming).unwrap());
    auto outgoing      = co_await lf::ByteQueue::make(context.resources(), exchange->window);
    if (outgoing.is_err()) co_return Err(rstd::move(outgoing).unwrap_err());
    exchange->outgoing = Some(rstd::move(outgoing).unwrap());
    exchange->ready.store(true);
    {
        auto f = exchange->fields.lock().unwrap();
        if (f->error.is_some()) co_return Err(f->error.take().unwrap());
    }
    auto producer = context.resources().spawn(UploadTask { exchange.clone() });
    if (producer.is_err()) co_return Err(rstd::move(producer).unwrap_err().error);
    exchange->producer = Some(rstd::move(producer).unwrap());
    co_return Ok(empty {});
}
auto wait_native(Arc<Exchange> exchange) -> coro<lf::Result<empty>> {
    for (;;) {
        auto event = exchange->changed.listen();
        if (exchange->fields.lock().unwrap()->done) co_return Ok(empty {});
        co_await rstd::move(event);
    }
}
struct Source {
    Arc<Exchange> exchange;
    auto          next() const { return exchange->incoming->next(); }
    auto          trailers() const -> lf::Result<Option<lihttpto::Headers>> {
        auto f = exchange->fields.lock().unwrap();
        if (f->failed) return Err(lf::Error::Closed());
        if (! f->done) return Ok(None());
        if (! f->trailers_available)
            return Err(lf::Error::Unsupported("backend does not expose response trailers"_Str));
        return Ok(Some(f->trailers.clone()));
    }
    void cancel() const { exchange->stop(); }
    auto close() const -> coro<lf::Result<empty>> {
        cancel();
        co_return co_await wait_native(exchange.clone());
    }
};
} // namespace ncrequest::client
namespace rstd
{
template<>
struct Impl<lifetchto::http::ResponseSource, ncrequest::client::Source>
    : ImplBase<ncrequest::client::Source> {
    auto next() const { return this->self().next(); }
    auto trailers() const { return this->self().trailers(); }
    void cancel() const { this->self().cancel(); }
    auto close() const { return this->self().close(); }
};
} // namespace rstd
namespace ncrequest::client
{
auto deliver(Arc<Exchange> exchange, lf::OutputSender<h::ResponseMessage> output)
    -> coro<lf::ProtocolCompletion> {
    Option<lihttpto::ResponseHead> head;
    Option<lf::Error>              pending_error;
    h::CacheObservation            cache;
    for (;;) {
        auto event = exchange->changed.listen();
        {
            auto f = exchange->fields.lock().unwrap();
            if (f->head.is_some()) {
                head          = f->head.take();
                cache         = f->cache;
                f->published  = true;
                pending_error = f->error.take();
            } else if (f->error.is_some())
                co_return lf::ProtocolCompletion { Err(f->error.take().unwrap()),
                                                   f->dispatched
                                                       ? lf::RemoteOutcome::Unknown
                                                       : lf::RemoteOutcome::NotDispatched };
        }
        if (head.is_some()) break;
        co_await rstd::move(event);
    }
    if (pending_error.is_some()) exchange->incoming->fail(pending_error.take().unwrap());
    auto body = h::ResponseBody::from_source(
        Arc<rstd::dyn<h::ResponseSource>>::make(Source { exchange.clone() }));
    auto control   = body.completion();
    auto published = output.publish({ head.take().unwrap(), rstd::move(body), cache });
    if (published.is_err()) exchange->stop();
    auto result = co_await control.wait();
    co_return lf::ProtocolCompletion { rstd::move(result), cache.remote(), control.failure() };
}
auto release_exchange(Arc<Exchange> exchange) -> coro<lf::Result<empty>> {
    exchange->stop();
    exchange->stop_upload();
    if (exchange->producer.is_some()) (void)co_await exchange->producer->wait();
    if (exchange->fields.lock().unwrap()->dispatched) (void)co_await wait_native(exchange.clone());
    auto closed = [&] {
        auto body = exchange->upload.lock().unwrap();
        return body->close();
    }();
    auto result = co_await rstd::move(closed);
    if (exchange->incoming.is_some()) exchange->incoming->release();
    if (exchange->outgoing.is_some()) exchange->outgoing->release();
    exchange->native_permit = None();
    co_return rstd::move(result);
}
auto prepare_exchange(h::BackendIdentity identity, lihttpto::RequestParts parts,
                      h::UploadMetadata upload, h::RouteChoice route, usize limit,
                      h::CacheDirective directive) -> lf::Result<h::PreparedExchange> {
    auto parsed = lihttpto::PreparedRequest::make(
        rstd::move(parts),
        upload.length,
        limit,
        upload.trailers.is_some() ? rstd::addressof(*upload.trailers) : nullptr);
    if (parsed.is_err()) return Err(lf::Error::InvalidOptions("invalid HTTP request metadata"_Str));
    auto scope = h::CacheScope { identity.clone(), route.clone() };
    return Ok(h::PreparedExchange::make(rstd::move(parsed).unwrap(),
                                        rstd::move(identity),
                                        rstd::move(upload),
                                        rstd::move(route),
                                        rstd::move(scope),
                                        directive));
}
} // namespace ncrequest::client
