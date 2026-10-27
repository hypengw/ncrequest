module ncrequest;
import :response;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest
{
struct BodyState {
    lifetchto::Transfer<lifetchto::http::ResponseMessage> transfer;
    lifetchto::http::ResponseBody                         body;
    usize                                                 limit;
    rstd::sync::atomic::Atomic<bool>                      busy { false }, started { false };
    BodyState(lifetchto::Transfer<lifetchto::http::ResponseMessage> transfer,
              lifetchto::http::ResponseBody body, usize limit)
        : transfer(rstd::move(transfer)), body(rstd::move(body)), limit(limit) {}
};
void stop_body(const Arc<BodyState>& state) {
    if (state) {
        state->body.cancel();
        state->transfer.control().cancel();
    }
}
struct BodyReadGuard {
    Arc<BodyState> state;
    bool           complete {};
    ~BodyReadGuard() {
        if (! complete) stop_body(state);
        state->busy.store(false);
    }
};
auto read_chunk(Arc<BodyState> state) -> coro<Result<Option<Bytes>>> {
    auto part = co_await state->body.next();
    if (part.is_err()) co_return Err(Error::Transport(rstd::move(part).unwrap_err()));
    if (part->is_none()) {
        auto report = co_await state->transfer.control().wait();
        if (report->logical.result.is_err())
            co_return Err(Error::Transport(report->logical.result.as_ref().unwrap_err().clone()));
    }
    co_return Ok(rstd::move(part).unwrap());
}
auto read_body(Arc<BodyState> state) -> coro<Result<Option<Bytes>>> {
    if (state->busy.exchange(true)) co_return Err(Error::Busy());
    BodyReadGuard guard { state.clone() };
    state->started.store(true);
    auto result    = co_await read_chunk(state.clone());
    guard.complete = result.is_ok();
    co_return rstd::move(result);
}
struct CollectionSource {
    using Error = ncrequest::Error;
    Arc<BodyState> state;
    auto           next() { return read_chunk(state.clone()); }
};
auto collect_body(Arc<BodyState> state, usize limit) -> coro<Result<Bytes>> {
    if (state->busy.exchange(true)) co_return Err(Error::Busy());
    BodyReadGuard guard { state.clone(), true };
    if (state->started.exchange(true)) co_return Err(Error::Consumed());
    guard.complete = false;
    limit          = rstd::cmp::min(limit, state->limit);
    auto result    = co_await lihttpto::collect_body(CollectionSource { state.clone() }, limit);
    if (result.is_err()) {
        stop_body(state);
        (void)co_await state->transfer.control().wait();
        auto error = rstd::move(result).unwrap_err();
        if (error.is_Source()) co_return Err(rstd::move(error.as_Source().error));
        co_return Err(Error::TooLarge(limit));
    }
    guard.complete = true;
    co_return Ok(rstd::move(result).unwrap());
}
auto text_body(Arc<BodyState> state, usize limit) -> coro<Result<String>> {
    auto result = co_await collect_body(rstd::move(state), limit);
    if (result.is_err()) co_return Err(rstd::move(result).unwrap_err());
    auto text = rstd::str_::from_utf8(result->as_slice());
    if (text.is_err()) co_return Err(Error::decode(rstd::move(text).unwrap_err()));
    co_return Ok(String::make(*text));
}
auto json_body(Arc<BodyState> state, usize limit) -> coro<Result<rstd::json::Value>> {
    auto result = co_await collect_body(rstd::move(state), limit);
    if (result.is_err()) co_return Err(rstd::move(result).unwrap_err());
    auto value = rstd::json::from_slice(result->as_slice());
    if (value.is_err()) co_return Err(Error::decode(rstd::move(value).unwrap_err()));
    co_return Ok(rstd::move(value).unwrap());
}
ResponseBody::ResponseBody(Arc<BodyState> state): state_(rstd::move(state)) {}
ResponseBody::ResponseBody(ResponseBody&&) noexcept = default;
ResponseBody::~ResponseBody() { cancel(); }
auto ResponseBody::operator=(ResponseBody&& other) noexcept -> ResponseBody& {
    if (this != &other) {
        cancel();
        state_ = rstd::move(other.state_);
    }
    return *this;
}
auto ResponseBody::next() -> coro<Result<Option<Bytes>>> { return read_body(state_.clone()); }
auto ResponseBody::collect() -> coro<Result<Bytes>> { return collect(state_->limit); }
auto ResponseBody::collect(usize limit) -> coro<Result<Bytes>> {
    return collect_body(state_.clone(), limit);
}
auto ResponseBody::text() -> coro<Result<String>> { return text(state_->limit); }
auto ResponseBody::text(usize limit) -> coro<Result<String>> {
    return text_body(state_.clone(), limit);
}
auto ResponseBody::json() -> coro<Result<rstd::json::Value>> { return json(state_->limit); }
auto ResponseBody::json(usize limit) -> coro<Result<rstd::json::Value>> {
    return json_body(state_.clone(), limit);
}
auto ResponseBody::trailers() const -> Result<Option<lihttpto::Headers>> {
    auto result = state_->body.trailers();
    if (result.is_err()) return Err(Error::Transport(rstd::move(result).unwrap_err()));
    return Ok(rstd::move(result).unwrap());
}
auto ResponseBody::completion() const -> lifetchto::TransferControl {
    return state_->transfer.control();
}
void ResponseBody::cancel() const { stop_body(state_); }
auto ResponseBody::close() const -> coro<Arc<lifetchto::TransferReport>> {
    return completion().close();
}
Response::Response(ConstructionKey, lihttpto::ResponseHead head, lihttpto::RequestParts request,
                   ResponseTiming timing, lifetchto::http::CacheObservation cache,
                   lifetchto::http::ExecutionObserver execution, ResponseBody body)
    : head_(rstd::move(head)),
      request_(rstd::move(request)),
      timing_(timing),
      cache_(cache),
      execution_(rstd::move(execution)),
      completion_(body.completion()),
      body_(Some(rstd::move(body))) {}
auto Response::make(lifetchto::Transfer<lifetchto::http::ResponseMessage> transfer,
                    lifetchto::http::ResponseMessage                      message,
                    lifetchto::http::ExecutionObserver execution, usize limit) -> Arc<Response> {
    auto facts = execution.snapshot();
    auto body =
        ResponseBody(Arc<BodyState>::make(rstd::move(transfer), rstd::move(message.body), limit));
    return Arc<Response>::make(ConstructionKey {},
                               rstd::move(message.head),
                               facts.request.take().unwrap(),
                               ResponseTiming { facts.started, facts.head_received },
                               message.cache,
                               rstd::move(execution),
                               rstd::move(body));
}
auto Response::take_body() -> Result<ResponseBody> {
    auto body = body_.lock().unwrap();
    if (body->is_none()) return Err(Error::BodyTaken());
    return Ok(body->take().unwrap());
}
auto Response::trailers() const -> Result<Option<lihttpto::Headers>> {
    auto body = body_.lock().unwrap();
    if (body->is_none()) return Err(Error::BodyTaken());
    return (**body).trailers();
}
auto Response::collect_bytes(ResponseBody body, Option<usize> limit) -> coro<Result<Bytes>> {
    co_return co_await (limit.is_some() ? body.collect(*limit) : body.collect());
}
auto Response::collect_text(ResponseBody body, Option<usize> limit) -> coro<Result<String>> {
    co_return co_await (limit.is_some() ? body.text(*limit) : body.text());
}
auto Response::collect_json(ResponseBody body, Option<usize> limit)
    -> coro<Result<rstd::json::Value>> {
    co_return co_await (limit.is_some() ? body.json(*limit) : body.json());
}
auto body_error_bytes(Error error) -> coro<Result<Bytes>> { co_return Err(rstd::move(error)); }
auto body_error_text(Error error) -> coro<Result<String>> { co_return Err(rstd::move(error)); }
auto body_error_json(Error error) -> coro<Result<rstd::json::Value>> {
    co_return Err(rstd::move(error));
}
auto Response::bytes() -> coro<Result<Bytes>> {
    auto body = take_body();
    if (body.is_err()) return body_error_bytes(rstd::move(body).unwrap_err());
    return collect_bytes(rstd::move(body).unwrap(), None());
}
auto Response::bytes(usize limit) -> coro<Result<Bytes>> {
    auto body = take_body();
    if (body.is_err()) return body_error_bytes(rstd::move(body).unwrap_err());
    return collect_bytes(rstd::move(body).unwrap(), Some(limit));
}
auto Response::text() -> coro<Result<String>> {
    auto body = take_body();
    if (body.is_err()) return body_error_text(rstd::move(body).unwrap_err());
    return collect_text(rstd::move(body).unwrap(), None());
}
auto Response::text(usize limit) -> coro<Result<String>> {
    auto body = take_body();
    if (body.is_err()) return body_error_text(rstd::move(body).unwrap_err());
    return collect_text(rstd::move(body).unwrap(), Some(limit));
}
auto Response::json() -> coro<Result<rstd::json::Value>> {
    auto body = take_body();
    if (body.is_err()) return body_error_json(rstd::move(body).unwrap_err());
    return collect_json(rstd::move(body).unwrap(), None());
}
auto Response::json(usize limit) -> coro<Result<rstd::json::Value>> {
    auto body = take_body();
    if (body.is_err()) return body_error_json(rstd::move(body).unwrap_err());
    return collect_json(rstd::move(body).unwrap(), Some(limit));
}
auto Response::set_cookies() const
    -> rstd::Result<Vec<lihttpto::SetCookie>, lihttpto::CookieError> {
    Vec<lihttpto::SetCookie> cookies;
    for (const auto& value : header().get_all("set-cookie"_str)) {
        auto parsed = lihttpto::SetCookie::parse_bytes(value->as_slice());
        if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
        cookies.push(rstd::move(parsed).unwrap());
    }
    return Ok(rstd::move(cookies));
}
} // namespace ncrequest
