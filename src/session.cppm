export module ncrequest:session;

export import :response;
export import :request_control;
export import :cache;
#if defined(LITO_FEAT_QT)
export import :client.qt_network.backend;
#else
export import :client.curl.session;
#endif
export import :client.http_backend;
import :redirect;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;

namespace ncrequest
{

#if defined(LITO_FEAT_QT)
using SelectedSessionBackend = client::qt_network::SessionBackend;
#else
using SelectedSessionBackend = client::curl::SessionBackend;
#endif

static_assert(client::HttpSessionBackend<SelectedSessionBackend, SelectedResponseBackend>);

export class Session {
    struct ConstructionKey {};
    struct State {
        EffectiveOptions           options;
        SelectedSessionBackend     backend;
        Option<CacheConfiguration> cache;
        Arc<Atomic<bool>>          closed = Arc<Atomic<bool>>::make(false);
        rstd::sync::Mutex<Vec<rstd::sync::Weak<RequestControl>>> cache_controls {
            Vec<rstd::sync::Weak<RequestControl>> {}
        };
        explicit State(EffectiveOptions value): options(rstd::move(value)) {}
#if defined(LITO_FEAT_QT)
        State(EffectiveOptions value, qt::QObject* parent)
            : options(rstd::move(value)), backend(parent) {}
        State(EffectiveOptions value, qt::QNetworkAccessManager* manager)
            : options(rstd::move(value)), backend(manager) {}
#endif
        void close() {
            if (! closed->exchange(true)) {
                {
                    auto controls = cache_controls.lock().unwrap();
                    for (auto& weak : *controls) {
                        auto active = weak.upgrade();
                        if (active) active->cancel();
                    }
                }
                backend.close();
            }
        }
    };
    Arc<State> state_;

public:
    Session(const Session&)                    = delete;
    auto operator=(const Session&) -> Session& = delete;

    Session(ConstructionKey, Arc<State> state): state_(rstd::move(state)) {}
    ~Session() { close(); }
    void close() { state_->close(); }

    static auto make(SessionOptions options = {}, Option<CacheConfiguration> cache = None())
        -> Result<Arc<Session>> {
        auto effective = EffectiveOptions::resolve(options, RequestOptions {});
        if (effective.is_err()) return Err(rstd::move(effective).unwrap_err());
        auto initialized = SelectedSessionBackend::initialize();
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto state   = Arc<State>::make(rstd::move(effective).unwrap());
        state->cache = rstd::move(cache);
        return finish_make(rstd::move(state));
    }
#if defined(LITO_FEAT_QT)
    static auto from_qt_parent(qt::QObject* parent) -> Result<Arc<Session>> {
        auto initialized = SelectedSessionBackend::initialize(parent);
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto options = EffectiveOptions::resolve(SessionOptions {}, RequestOptions {}).unwrap();
        return finish_make(Arc<State>::make(rstd::move(options), parent));
    }
    static auto from_qt_manager(qt::QNetworkAccessManager* manager) -> Result<Arc<Session>> {
        auto initialized = SelectedSessionBackend::initialize(manager);
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto options = EffectiveOptions::resolve(SessionOptions {}, RequestOptions {}).unwrap();
        return finish_make(Arc<State>::make(rstd::move(options), manager));
    }
#endif

    auto get(Request req) -> coro<Result<Arc<Response>>> {
        req.set_method(lihttpto::Method::parse("GET"_str).unwrap());
        return send(rstd::move(req));
    }

    auto post(Request req) -> coro<Result<Arc<Response>>> {
        req.set_method(lihttpto::Method::parse("POST"_str).unwrap());
        return send(rstd::move(req));
    }

    auto post(Request req, Bytes body) -> coro<Result<Arc<Response>>> {
        req.set_body(rstd::move(body));
        return post(rstd::move(req));
    }

    auto send(Request req) -> coro<Result<Arc<Response>>> {
        return send_request(state_.clone(), rstd::move(req));
    }

    auto send(Request req, Arc<RequestControl> control) -> coro<Result<Arc<Response>>> {
        return send_request(state_.clone(), rstd::move(req), Some(rstd::move(control)));
    }

private:
    static auto finish_make(Arc<State> state) -> Result<Arc<Session>> {
        auto started = state->backend.start();
        if (started.is_err()) return Err(rstd::move(started).unwrap_err());
        return Ok(Arc<Session>::make(ConstructionKey {}, rstd::move(state)));
    }

    static auto send_request(Arc<State> state, Request req,
                             Option<Arc<RequestControl>> control = None())
        -> coro<Result<Arc<Response>>> {
        if (control.is_some()) {
            if (! *control) co_return Err(Error::InvalidState("request control is null"));
            if (! (*control)->claim())
                co_return Err(Error::InvalidState("request control was already used"));
            if ((*control)->is_canceled()) co_return Err(Error::Canceled());
#if defined(LITO_FEAT_QT)
            co_return Err(Error::Unsupported("Qt Network request control is not supported"));
#endif
        }
        if (state->closed->load()) co_return Err(Error::Canceled());
        auto effective = state->options.with_request(req.options());
        if (effective.is_err()) co_return Err(rstd::move(effective).unwrap_err());
        auto prepared = PreparedRequest::prepare(rstd::move(req), rstd::move(effective).unwrap());
        if (prepared.is_err()) co_return Err(rstd::move(prepared).unwrap_err());
        if (state->cache.is_none()) {
            if (prepared->request().options().cache_mode == CacheMode::OnlyIfCached)
                co_return Err(Error::InvalidState("cache not configured"));
            co_return co_await send_network(
                state.clone(), rstd::move(prepared).unwrap(), rstd::move(control));
        }
        auto& configuration = *state->cache;
        auto  mode          = prepared->request().options().cache_mode;
        auto  requested     = lihttpto::CacheControl::parse(prepared->request().header());
        auto& headers       = prepared->request().header();
        bool  eligible =
            prepared->request().method().as_ref() == "GET"_str &&
            prepared->request().url_info().userinfo().is_none() && mode != CacheMode::Bypass &&
            prepared->request().body().bytes().is_none() &&
            prepared->request().body().reader().is_none() &&
            prepared->request().body().stream().is_none() && ! requested.no_store &&
            ! requested.invalid && ! headers.contains("range"_str) &&
            ! headers.contains("cookie"_str) && ! headers.contains("if-none-match"_str) &&
            ! headers.contains("if-modified-since"_str) && ! headers.contains("if-match"_str) &&
            ! headers.contains("if-unmodified-since"_str) &&
            ! prepared->options().redirect().enabled();
        if (! eligible) {
            if (mode == CacheMode::OnlyIfCached)
                co_return Err(Error::InvalidState("no reusable cache entry"));
            co_return co_await send_network(
                state.clone(), rstd::move(prepared).unwrap(), rstd::move(control));
        }
        auto original = prepared->request().try_clone();
        if (original.is_err()) co_return Err(rstd::move(original).unwrap_err());
        auto active = control.is_some() ? control->clone() : Arc<RequestControl>::make();
        {
            auto controls = state->cache_controls.lock().unwrap();
            for (usize i = controls->len(); i > usize(); --i)
                if (! (*controls)[i - usize(1)].upgrade()) controls->remove(i - usize(1));
            controls->push(active.downgrade());
            if (state->closed->load()) active->cancel();
        }
        auto origin = original->url_info().http_origin();
        auto url =
            origin.is_some()
                ? rstd::format("{}{}", origin->serialize(), original->url_info().request_target())
                : String::make(original->url());
        auto found = co_await configuration.storage->open(
            CacheQuery { String::make(configuration.scope.as_str()),
                         rstd::move(url),
                         original->header().clone() },
            active.clone());
        if (active->is_canceled() || state->closed->load()) co_return Err(Error::Canceled());
        if (found.is_err()) {
            configuration.storage->report(found.unwrap_err().kind());
            if (mode == CacheMode::OnlyIfCached)
                co_return Err(Error::InvalidState("cache entry unavailable"));
            co_return co_await send_network(
                state.clone(), rstd::move(prepared).unwrap(), rstd::move(control));
        }
        auto access = rstd::move(found).unwrap();
        struct WriteGuard {
            Option<CacheWriter>* writer;
            ~WriteGuard() {
                if (writer->is_some()) (**writer)->abort();
            }
        } guard { &access.writer };
        auto limit = prepared->options().limits().collect_bytes;
        if (access.record.is_some() && mode != CacheMode::Validate &&
            access.record->metadata.policy.fresh(cache_now(), requested)) {
            auto record      = access.record.take().unwrap();
            auto current_age = rstd::format("{}", record.metadata.policy.age(cache_now()));
            record.metadata.head.headers.remove("age"_str);
            record.metadata.head.headers.push(
                lihttpto::Header::make("age"_str, current_age.as_str().as_bytes()).unwrap());
            auto response = Response::from_source(
                ResponseSource::make(CachedReader {
                    rstd::move(record.body), active.clone(), state->closed.clone() }),
                rstd::move(original).unwrap(),
                rstd::move(record.metadata.head),
                limit);
            response->set_timing(
                { cache_now(), cache_now(), true, false, record.metadata.policy.date_anchor });
            co_return Ok(rstd::move(response));
        }
        if (mode == CacheMode::OnlyIfCached)
            co_return Err(Error::InvalidState("no fresh cache entry"));
        bool conditional {};
        if (access.record.is_some()) {
            auto& fields = access.record->metadata.policy.fields;
            if (fields.etag.is_some()) {
                (void)original->try_set_header("if-none-match"_str, fields.etag->as_str());
                conditional = true;
            }
            auto modified =
                lihttpto::header_text(access.record->metadata.head.headers, "last-modified"_str);
            if (fields.last_modified.is_some() && modified.is_some()) {
                (void)original->try_set_header("if-modified-since"_str, *modified);
                conditional = true;
            }
            auto replacement = original->try_clone();
            if (replacement.is_err()) co_return Err(rstd::move(replacement).unwrap_err());
            prepared = PreparedRequest::prepare(rstd::move(replacement).unwrap(),
                                                prepared->options().clone());
            if (prepared.is_err()) co_return Err(rstd::move(prepared).unwrap_err());
        }
        auto requested_at = cache_now();
        auto response     = co_await send_network(
            state.clone(), rstd::move(prepared).unwrap(), Some(active.clone()));
        if (response.is_err()) co_return Err(rstd::move(response).unwrap_err());
        auto received_at = cache_now();
        if (conditional && (*response)->code() == Some(i32(304))) {
            auto record = access.record.take().unwrap();
            record.metadata.head.headers =
                lihttpto::merge_not_modified(record.metadata.head.headers, (*response)->header());
            record.metadata.policy =
                CachePolicy::make(record.metadata.head.headers, requested_at, received_at);
            if (access.writer.is_some()) {
                auto updated = co_await (*access.writer)->publish(record.metadata.clone(), true);
                if (updated.is_err())
                    configuration.storage->report(updated.unwrap_err().kind());
                else
                    access.writer = None();
            }
            (*response)->cancel();
            auto cached = Response::from_source(
                ResponseSource::make(CachedReader {
                    rstd::move(record.body), active.clone(), state->closed.clone() }),
                rstd::move(original).unwrap(),
                rstd::move(record.metadata.head),
                limit);
            cached->set_timing(
                { requested_at, received_at, true, true, record.metadata.policy.date_anchor });
            co_return Ok(rstd::move(cached));
        }
        auto policy = CachePolicy::make((*response)->header(), requested_at, received_at);
        if ((*response)->code() == Some(i32(200)) && ! policy.fields.reusable &&
            access.writer.is_some()) {
            auto& head      = (*response)->head();
            auto  discarded = co_await (*access.writer)
                                  ->publish(CacheMetadata { { head.status,
                                                              head.headers.clone(),
                                                              head.version,
                                                              head.reason.clone() },
                                                            rstd::move(policy) },
                                            true);
            if (discarded.is_err())
                configuration.storage->report(discarded.unwrap_err().kind());
            else
                access.writer = None();
            co_return response;
        }
        if ((*response)->code() != Some(i32(200)) || ! policy.fields.reusable ||
            access.writer.is_none())
            co_return response;
        auto& head     = (*response)->head();
        auto  metadata = CacheMetadata {
            { head.status, head.headers.clone(), head.version, head.reason.clone() },
            rstd::move(policy)
        };
        auto body = (*response)->take_body();
        if (body.is_err()) co_return Err(rstd::move(body).unwrap_err());
        auto source  = ResponseSource::make(WritingReader { rstd::move(body).unwrap(),
                                                            response->clone(),
                                                            access.writer.take().unwrap(),
                                                            configuration.storage.clone(),
                                                            metadata.clone() });
        auto wrapped = Response::from_source(
            rstd::move(source), rstd::move(original).unwrap(), rstd::move(metadata.head), limit);
        wrapped->set_timing((*response)->timing());
        co_return Ok(rstd::move(wrapped));
    }
    static auto send_network(Arc<State> state, PreparedRequest input,
                             Option<Arc<RequestControl>> control) -> coro<Result<Arc<Response>>> {
        auto prepared  = Ok(rstd::move(input));
        auto redirects = RedirectState(prepared->options().clone());
        for (;;) {
            if (state->closed->load()) co_return Err(Error::Canceled());
            if (control.is_some() && (*control)->is_canceled()) co_return Err(Error::Canceled());
            auto limits  = prepared->options().limits();
            auto started = cache_now();
#if defined(LITO_FEAT_QT)
            auto res = co_await state->backend.start_request(rstd::move(prepared).unwrap());
#else
            auto res = co_await state->backend.start_request(
                rstd::move(prepared).unwrap(),
                control.is_some() ? Some(control->clone()) : None<Arc<RequestControl>>());
#endif
            if (res.is_err()) co_return Err(rstd::move(res).unwrap_err());
            auto backend = rstd::move(res).unwrap();
            auto ready   = co_await backend.ready_head();
            if (ready.is_err()) co_return Err(rstd::move(ready).unwrap_err());
            auto head = backend.head();
            if (head.is_none()) co_return Err(Error::InvalidState("response head is unavailable"));
            auto next = redirects.next(backend.request(), **head);
            if (next.is_err()) co_return Err(rstd::move(next).unwrap_err());
            if (next->is_none()) {
                auto response = Response::make(rstd::move(backend), limits);
                if (response.is_ok())
                    (*response)->set_timing({ started, cache_now(), false, false });
                co_return response;
            }
            backend.cancel();
            prepared = Ok(rstd::move(next).unwrap().unwrap());
        }
    }
};

} // namespace ncrequest
