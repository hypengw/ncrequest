#pragma once
namespace test_http
{
namespace lf = lifetchto;
namespace h  = lifetchto::http;
using namespace rstd::prelude;
using namespace rstd::literals;
using lf::Arc;
using lf::Bytes;
using lf::coro;
using lf::Duration;
struct Reply {
    u16               status { 200 };
    lihttpto::Headers headers;
    Bytes             data { Bytes::copy_from_slice("payload"_str.as_bytes()) };
    Duration          head_delay, body_delay;
    bool              truncated {};
};
struct State {
    Vec<Reply>                  replies;
    Vec<lihttpto::RequestParts> requests;
    Vec<Bytes>                  uploads;
    Option<lf::Instant>         deadline;
    usize                       prepared {}, releases {}, backend_closes {};
    bool                        closed {}, fail_close {};
    Duration                    release_delay;
};
using Script = Arc<rstd::sync::Mutex<State>>;
struct Cursor {
    bool             sent {};
    lf::Cancellation cancellation;
};
struct Source {
    Script                         script;
    Arc<rstd::sync::Mutex<Cursor>> cursor;
    Bytes                          data;
    Duration                       delay;
    bool                           truncated;
    auto                           next() const -> coro<lf::Result<Option<Bytes>>> {
        lf::Cancellation stop;
        bool             sent;
        {
            auto f  = cursor->lock().unwrap();
            stop    = f->cancellation.clone();
            sent    = f->sent;
            f->sent = true;
        }
        if (stop.is_canceled()) co_return Err(lf::Error::Canceled());
        if (sent) {
            if (truncated) co_return Err(lf::Error::BodySource("truncated"_Str));
            co_return Ok(None());
        }
        if (! delay.is_zero()) {
            auto ready = co_await rstd::async::select(rstd::async::sleep(delay),
                                                      stop.stopped().into_future());
            if (ready.is_right()) co_return Err(lf::Error::Canceled());
        }
        co_return Ok(Some(data.clone()));
    }
    auto trailers() const -> lf::Result<Option<lihttpto::Headers>> {
        return Ok(Some(lihttpto::Headers {}));
    }
    void cancel() const { cursor->lock().unwrap()->cancellation.cancel(); }
    auto close() const -> coro<lf::Result<empty>> {
        cancel();
        co_return Ok(empty {});
    }
};
struct DriverState {
    rstd::sync::Mutex<Option<h::BodyControl>> body { None<h::BodyControl>() };
};
struct Driver {
    h::ExchangeInput input;
    Script           script;
    Arc<DriverState> state { Arc<DriverState>::make() };
    auto             run(lf::TransferContext, lf::OutputSender<h::ResponseMessage>)
        -> coro<lf::ProtocolCompletion>;
    void request_stop(lf::StopReason reason) const {
        input.body.cancel();
        auto f = state->body.lock().unwrap();
        if (f->is_some()) (*f)->cancel(reason);
    }
    auto release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        auto     closed = co_await input.body.close();
        Duration delay;
        {
            auto f = script->lock().unwrap();
            ++f->releases;
            delay = f->release_delay;
        }
        if (! delay.is_zero()) co_await rstd::async::sleep(delay);
        co_return rstd::move(closed);
    }
};
struct Backend {
    Script             script;
    h::BackendIdentity identity;
    auto prepare(lihttpto::RequestParts parts, h::UploadMetadata upload, h::RouteChoice route, h::CacheDirective directive) const
        -> lf::Result<h::PreparedExchange> {
        ++script->lock().unwrap()->prepared;
        auto parsed = lihttpto::PreparedRequest::make(
            rstd::move(parts),
            upload.length,
            usize(65536),
            upload.trailers.is_some() ? rstd::addressof(*upload.trailers) : nullptr);
        if (parsed.is_err()) return Err(lf::Error::InvalidOptions("invalid request"_Str));
        auto cache = h::CacheScope { identity.clone(), route.clone() };
        return Ok(h::PreparedExchange::make(rstd::move(parsed).unwrap(),
                                            identity.clone(),
                                            rstd::move(upload),
                                            rstd::move(route),
                                            rstd::move(cache), directive));
    }
    auto operation(h::ExchangeInput input) const
        -> rstd::Result<lf::PreparedOperation<h::ResponseMessage>, lf::Rejected<h::ExchangeInput>> {
        return Ok(lf::PreparedOperation<h::ResponseMessage>::make(
            Driver { rstd::move(input), script.clone() }));
    }
    void request_close() const { script->lock().unwrap()->closed = true; }
    auto close() const -> coro<lf::Result<empty>> {
        auto f    = script->lock().unwrap();
        f->closed = true;
        ++f->backend_closes;
        if (f->fail_close)
            co_return Err(lf::Error::Extension(
                "backend"_Str,
                Box<rstd::dyn<rstd::error::Error>>::make(rstd::io::Error::from_kind(
                    rstd::io::ErrorKind { rstd::io::ErrorKind::Other }))));
        co_return Ok(empty {});
    }
};
} // namespace test_http
namespace rstd
{
template<>
struct Impl<lifetchto::http::ResponseSource, test_http::Source> : ImplBase<test_http::Source> {
    auto next() const { return this->self().next(); }
    auto trailers() const { return this->self().trailers(); }
    void cancel() const { this->self().cancel(); }
    auto close() const { return this->self().close(); }
};
template<>
struct Impl<lifetchto::http::ExchangeBackend, test_http::Backend> : ImplBase<test_http::Backend> {
    auto duplex(lifetchto::http::DuplexRequest input) const -> rstd::Result<lifetchto::PreparedOperation<lifetchto::DuplexStream>, lifetchto::Rejected<lifetchto::http::DuplexRequest>> {
        return Err(lifetchto::Rejected<lifetchto::http::DuplexRequest> { lifetchto::Error::Unsupported("test backend has no duplex transport"_Str), rstd::move(input) });
    }
    auto capabilities() const -> lifetchto::http::BackendCapabilities {
        return { .http1 = true, .https = true, .unknown_upload_length = true, .upload_trailers = true, .response_trailers = true, .repeated_request_fields = true, .expect_continue = true };
    }
    auto prepare(lihttpto::RequestParts p, lifetchto::http::UploadMetadata u,
                 lifetchto::http::RouteChoice r, lifetchto::http::CacheDirective directive) const {
        return this->self().prepare(rstd::move(p), rstd::move(u), rstd::move(r), directive);
    }
    auto operation(lifetchto::http::ExchangeInput i) const {
        return this->self().operation(rstd::move(i));
    }
    void request_close() const { this->self().request_close(); }
    auto close() const { return this->self().close(); }
};
} // namespace rstd
namespace test_http
{
inline auto Driver::run(lf::TransferContext context, lf::OutputSender<h::ResponseMessage> output)
    -> coro<lf::ProtocolCompletion> {
    auto attempt = context.begin_attempt();
    if (attempt.is_err())
        co_return lf::ProtocolCompletion { Err(rstd::move(attempt).unwrap_err()),
                                           lf::RemoteOutcome::NotDispatched };
    Reply reply;
    {
        auto f     = script->lock().unwrap();
        auto index = f->requests.len();
        f->requests.push(input.prepared.parts().clone());
        f->deadline = context.deadline();
        if (index < f->replies.len()) reply = rstd::move(f->replies[index]);
    }
    Vec<u8> bytes;
    for (;;) {
        auto part = co_await input.body.next(usize(32));
        if (part.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(part).unwrap_err()),
                                               lf::RemoteOutcome::Unknown };
        if (part->is_none()) break;
        bytes.extend_from_slice((**part).as_slice());
    }
    script->lock().unwrap()->uploads.push(Bytes::copy_from_slice(bytes.as_slice()));
    if (! reply.head_delay.is_zero()) {
        auto delayed = co_await context.delay(reply.head_delay);
        if (delayed.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(delayed).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
    }
    auto body    = h::ResponseBody::from_source(Arc<rstd::dyn<h::ResponseSource>>::make(
        Source { script.clone(),
                 Arc<rstd::sync::Mutex<Cursor>>::make(Cursor {}),
                 rstd::move(reply.data),
                 reply.body_delay,
                 reply.truncated }));
    auto control = body.completion();
    *state->body.lock().unwrap() = Some(control.clone());
    auto published               = output.publish(
        { { lihttpto::StatusCode::make(reply.status).unwrap(), rstd::move(reply.headers), {}, {} },
          rstd::move(body) });
    if (published.is_err()) control.cancel();
    auto done = co_await control.wait();
    co_return lf::ProtocolCompletion { rstd::move(done), lf::RemoteOutcome::Confirmed };
}
inline auto backend(const Script& script) -> Arc<rstd::dyn<h::ExchangeBackend>> {
    return Arc<rstd::dyn<h::ExchangeBackend>>::make(Backend { script.clone(), {} });
}
inline auto request(ref<str> url = "http://example.test/item"_str) -> ncrequest::Request {
    return ncrequest::Request::from_url(url).unwrap();
}
inline auto response(ncrequest::Session& session, ncrequest::Request request)
    -> coro<ncrequest::Result<Arc<ncrequest::Response>>> {
    auto sent = session.send(rstd::move(request));
    if (sent.is_err()) {
        auto rejected = rstd::move(sent).unwrap_err();
        (void)co_await rejected.input.close();
        co_return Err(ncrequest::Error::Transport(rstd::move(rejected.error)));
    }
    auto pending = rstd::move(sent).unwrap();
    co_return co_await pending.response();
}
} // namespace test_http
