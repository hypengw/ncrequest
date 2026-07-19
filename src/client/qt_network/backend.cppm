module;
#include <rstd/enum.hpp>
#include <utility>

export module ncrequest:client_qt_network;
export import :qt;
export import :request;
export import :http;
export import :error;
export import ncrequest.coro;
export import ncrequest.type;
import :session_share_backend;

namespace ncrequest::client::qt_network
{

using namespace ncrequest::qt;
using rstd::collections::HashMap;
using rstd::sync::atomic::Atomic;

export struct Options {};

export class ResponseBackend;
class QtNetworkDriver;

// Qt copies queued and signal functors, so CloneTuple adapts explicit Arc cloning to that boundary.
class QtExecutor {
    QPointer<QObject> m_target;

public:
    explicit QtExecutor(QObject* target): m_target(target) {}

    auto post_job(rstd::async::ExecutorJob job) -> bool {
        auto* target = m_target.data();
        if (target == nullptr) return false;
        auto owned = Arc<rstd::async::ExecutorJob>::make(rstd::move(job));
        return QMetaObject::invokeMethod(
            target,
            [capture = make_clone_tuple(rstd::move(owned))]() mutable {
                capture.get<0>()->run();
            },
            Qt::QueuedConnection);
    }

    auto is_closed() -> bool { return m_target.isNull(); }
};

} // namespace ncrequest::client::qt_network

template<>
struct rstd::Impl<rstd::async::Executor, ncrequest::client::qt_network::QtExecutor>
    : rstd::LinkClassMethod<rstd::async::Executor, ncrequest::client::qt_network::QtExecutor> {};

namespace ncrequest::client::qt_network
{

using namespace ncrequest::qt;

auto make_qt_executor(QObject* target) -> rstd::Option<rstd::async::AnyExecutor> {
    if (target == nullptr) return None<rstd::async::AnyExecutor>();
    return Some(rstd::async::AnyExecutor::from_executor(QtExecutor { target }));
}

#define NCREQUEST_QT_BODY_EVENT_VARIANTS(V) \
    V(Header, (http::MessageHead value;))   \
    V(Chunk, (rstd::bytes::Bytes value;))   \
    V(Finished, ())                         \
    V(Failed, (Error value;))

struct BodyEvent {
    RSTD_ENUM_BODY(BodyEvent, NCREQUEST_QT_BODY_EVENT_VARIANTS)
};

#undef NCREQUEST_QT_BODY_EVENT_VARIANTS

struct DirectReplyState {
    QPointer<QNetworkReply> reply;
};

struct OperationState {
    Request                                            request;
    http::Operation                                    operation;
    Weak<QtNetworkDriver>                              driver;
    rstd::Option<rstd::async::AnyExecutor>             executor;
    Option<Arc<DirectReplyState>>                      direct;
    rstd::Option<req_opt::Proxy>                       proxy;
    rstd::async::CompletionHandle<rstd::Option<Error>> ready;
    rstd::async::CompletionQueueHandle<BodyEvent>      body;
    Atomic<bool>                                       finished { false };
    Atomic<bool>                                       cancel_requested { false };

    OperationState(Request request, http::Operation operation, Weak<QtNetworkDriver> driver,
                   rstd::Option<rstd::async::AnyExecutor>             executor,
                   rstd::Option<req_opt::Proxy>                       proxy,
                   rstd::async::CompletionHandle<rstd::Option<Error>> ready,
                   rstd::async::CompletionQueueHandle<BodyEvent>      body)
        : request(rstd::move(request)),
          operation(operation),
          driver(rstd::move(driver)),
          executor(rstd::move(executor)),
          proxy(rstd::move(proxy)),
          ready(rstd::move(ready)),
          body(rstd::move(body)) {}

    void cancel();

    void complete_ready(rstd::Option<Error> error = None<Error>()) {
        (void)ready.complete(rstd::move(error));
    }

    void push_body_event(BodyEvent event) {
        auto close = event.is_Finished() || event.is_Failed();
        (void)body.push(rstd::move(event));
        if (close) body.close();
    }

    void close_body() { body.close(); }
};

auto make_qnetwork_request(const Request& req) -> Result<QNetworkRequest> {
    if (QCoreApplication::instance() == nullptr) {
        return Err(Error::InvalidState("Qt network backend requires a QCoreApplication"));
    }

    QNetworkRequest request { QUrl(QString::fromUtf8(req.url().data(), req.url().size())) };
    request.setAttribute(QNetworkRequest::AutoDeleteReplyOnFinishAttribute, false);

    auto raw_headers = QList<std::pair<QByteArray, QByteArray>> {};
    raw_headers.reserve(static_cast<qsizetype>(req.header().len().to_primitive()));
    auto fields = req.header().iter();
    for (auto field = fields.next(); field.is_some(); field = fields.next()) {
        auto name        = (**field).name().as_ref();
        auto value       = (**field).value().as_bytes();
        auto value_bytes = rstd::as_bytes(value);
        if (req.header().values(name).count() > usize(1)) {
            return Err(
                Error::Unsupported("Qt Network cannot preserve repeated request header fields"));
        }
        raw_headers.append(
            { QByteArray(reinterpret_cast<const char*>(name.data()),
                         static_cast<qsizetype>(name.size().to_primitive())),
              QByteArray(reinterpret_cast<const char*>(value_bytes.as_raw_ptr()),
                         static_cast<qsizetype>(value_bytes.len().to_primitive())) });
    }
    request.setHeaders(QHttpHeaders::fromListOfPairs(raw_headers));

    auto const& timeout = req.get_opt<req_opt::Timeout>();
    if (timeout.transfer_timeout > i64()) {
        request.setTransferTimeout(static_cast<int>(timeout.transfer_timeout.to_primitive()));
    }

    auto const& ssl = req.get_opt<req_opt::SSL>();
    if (! ssl.verify_certificate) {
        auto ssl_config = request.sslConfiguration();
        ssl_config.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(ssl_config);
    }

    return Ok(rstd::move(request));
}

void apply_proxy(QNetworkAccessManager* manager, const req_opt::Proxy& proxy) {
    if (manager == nullptr) return;

    if (proxy.content.empty()) {
        manager->setProxy(QNetworkProxy { QNetworkProxy::NoProxy });
        return;
    }

    QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
    switch (proxy.type) {
    case req_opt::Proxy::Type::SOCKS4:
    case req_opt::Proxy::Type::SOCKS4A:
    case req_opt::Proxy::Type::SOCKS5:
    case req_opt::Proxy::Type::SOCKS5H: type = QNetworkProxy::Socks5Proxy; break;
    case req_opt::Proxy::Type::HTTP:
    case req_opt::Proxy::Type::HTTPS2: type = QNetworkProxy::HttpProxy; break;
    }

    auto raw  = QString::fromStdString(proxy.content);
    auto url  = QUrl::fromUserInput(raw);
    auto host = url.host();
    auto port = url.port();

    if (host.isEmpty()) {
        host       = raw;
        auto colon = raw.lastIndexOf(':');
        if (colon > 0) {
            bool ok = false;
            port    = raw.mid(colon + 1).toInt(&ok);
            if (ok) {
                host = raw.left(colon);
            }
        }
    }

    auto qproxy = QNetworkProxy { type, host, static_cast<quint16>(port > 0 ? port : 0) };
    manager->setProxy(qproxy);
}

auto send_request(QNetworkAccessManager* manager, QNetworkRequest request,
                  http::Operation operation, rstd::Option<rstd::bytes::Bytes> body)
    -> QNetworkReply* {
    if (manager == nullptr) return nullptr;
    if (! operation.is_Post()) {
        return manager->get(rstd::move(request));
    }

    if (! request.hasRawHeader("Content-Type")) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    }

    auto payload = QByteArray {};
    if (body.is_some()) {
        auto bytes = rstd::move(body).unwrap();
        auto raw   = rstd::as_bytes(bytes.as_slice());
        payload    = QByteArray(reinterpret_cast<const char*>(raw.as_raw_ptr()),
                                static_cast<qsizetype>(raw.len().to_primitive()));
    }
    return manager->post(request, payload);
}

class QtNetworkManagerRouter : public QObject {
    using ShareKey = const void*;

    QPointer<QNetworkAccessManager>                    m_default_manager;
    HashMap<ShareKey, QPointer<QNetworkAccessManager>> m_share_managers;
    bool                                               m_allow_share;

public:
    QtNetworkManagerRouter(QNetworkAccessManager* default_manager, bool allow_share,
                           QObject* parent = nullptr)
        : QObject(parent), m_default_manager(default_manager), m_allow_share(allow_share) {}

    auto manager_for(const Request& request) -> Result<QNetworkAccessManager*> {
        auto* default_manager = m_default_manager.data();
        if (default_manager == nullptr) {
            return Err(Error::InvalidState("QNetworkAccessManager is not available"));
        }

        cleanup();
        auto const& share = request.get_opt<req_opt::Share>().share;
        if (share.is_none()) return Ok(default_manager);
        if (! m_allow_share) {
            return Err(Error::InvalidState(
                "request Share is not supported with an external QNetworkAccessManager"));
        }

        auto key      = detail::SessionShareAccess::token(*share);
        auto existing = m_share_managers.get(key);
        if (existing.is_some()) {
            if (auto* manager = (**existing).data()) return Ok(manager);
            (void)m_share_managers.remove(key);
        }

        auto* manager = new QNetworkAccessManager(this);
        manager->setRedirectPolicy(default_manager->redirectPolicy());
        manager->setCookieJar(detail::SessionShareAccess::make_cookie_jar(*share, manager));
        (void)m_share_managers.insert(key, QPointer<QNetworkAccessManager>(manager));
        return Ok(manager);
    }

private:
    void cleanup() {
        m_share_managers.retain([](const ShareKey&, QPointer<QNetworkAccessManager>& owner) {
            auto* manager = owner.data();
            if (manager != nullptr &&
                ! detail::SessionShareAccess::cookie_jar_expired(manager->cookieJar())) {
                return true;
            }
            if (manager != nullptr) manager->deleteLater();
            return false;
        });
    }
};

auto read_header(QNetworkReply* reply)
    -> rstd::Result<rstd::Option<http::MessageHead>, http::HttpParseError> {
    if (reply == nullptr) return rstd::Ok(rstd::None<http::MessageHead>());

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (! status.isValid()) return rstd::Ok(rstd::None<http::MessageHead>());

    auto status_code = http::StatusCode::make(static_cast<rstd::u16>(status.toInt()));
    if (status_code.is_err()) return rstd::Err(rstd::move(status_code).unwrap_err());

    auto headers = http::Header {};

    for (auto const& pair : reply->headers().toListOfPairs()) {
        auto name_text = rstd::ref<rstd::str>::from_raw_parts(
            reinterpret_cast<const rstd::byte*>(pair.first.constData()),
            static_cast<rstd::usize>(pair.first.size()));
        auto name = http::HeaderName::parse(name_text);
        if (name.is_err()) {
            return rstd::Err(http::HttpParseError { http::HttpParseErrorKind::InvalidHeaderLine(),
                                                    name.unwrap_err().offset() });
        }

        auto value = http::HeaderValue::from_bytes(rstd::slice<rstd::byte>::from_raw_parts(
            reinterpret_cast<const rstd::byte*>(pair.second.constData()),
            static_cast<rstd::usize>(pair.second.size())));
        if (value.is_err()) {
            return rstd::Err(http::HttpParseError { http::HttpParseErrorKind::InvalidHeaderLine(),
                                                    value.unwrap_err().offset() });
        }
        headers.append(http::HeaderField { rstd::move(name).unwrap(), rstd::move(value).unwrap() });
    }

    auto start = http::StartLine::Response(http::StatusLine { rstd::None<http::Version>(),
                                                              rstd::move(status_code).unwrap(),
                                                              rstd::None<http::HeaderValue>() });
    return rstd::Ok(rstd::Some(http::MessageHead { rstd::move(start), rstd::move(headers) }));
}

auto transport_error(QNetworkReply* reply) -> rstd::Option<Error> {
    if (reply == nullptr) {
        return Some(Error::InvalidState("QNetworkReply was destroyed"));
    }

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    auto error  = reply->error();
    if (error == QNetworkReply::NoError) return None<Error>();
    if (status.isValid()) return None<Error>();
    if (error == QNetworkReply::OperationCanceledError) {
        return Some(Error::Canceled());
    }

    auto message = reply->errorString().toStdString();
    return Some(Error::Client(ClientError {
        .backend = ClientBackend::QtNetwork,
        .code    = static_cast<i32>(error),
        .message = rstd::move(message),
    }));
}

void publish_header(const Arc<OperationState>& state, QNetworkReply* reply) {
    if (state->finished.load()) return;
    auto header = read_header(reply);
    if (header.is_err()) {
        auto error    = rstd::move(header).unwrap_err();
        auto protocol = error.kind().is_InvalidStartLine() ? ProtocolError::InvalidStatusLine
                                                           : ProtocolError::InvalidHeaderLine;
        state->push_body_event(
            BodyEvent::Failed(Error::Protocol(protocol, protocol_error_message(protocol))));
        return;
    }
    auto value = rstd::move(header).unwrap();
    if (value.is_some()) {
        state->push_body_event(BodyEvent::Header(rstd::move(value).unwrap()));
    }
}

void publish_chunks(const Arc<OperationState>& state, QNetworkReply* reply) {
    if (state->finished.load() || reply == nullptr) return;

    while (reply->bytesAvailable() > 0) {
        auto chunk = reply->read(reply->bytesAvailable());
        if (chunk.isEmpty()) break;

        auto bytes = rstd::bytes::Bytes::copy_from_bytes(rstd::slice<rstd::byte>::from_raw_parts(
            reinterpret_cast<const rstd::byte*>(chunk.constData()),
            static_cast<rstd::usize>(chunk.size())));
        state->push_body_event(BodyEvent::Chunk(rstd::move(bytes)));
    }
}

void publish_finished(const Arc<OperationState>& state, QNetworkReply* reply) {
    if (state->finished.load()) return;

    publish_header(state, reply);
    publish_chunks(state, reply);
    state->finished.store(true);

    auto error = transport_error(reply);
    if (error.is_some()) {
        state->push_body_event(BodyEvent::Failed(rstd::move(error).unwrap()));
    } else {
        state->push_body_event(BodyEvent::Finished());
    }
}

class QtNetworkWorker : public QObject {
    struct ReplyEntry {
        QPointer<QNetworkReply> reply;
        Arc<OperationState>     state;
    };

    QNetworkAccessManager*               m_manager { nullptr };
    QtNetworkManagerRouter*              m_router { nullptr };
    HashMap<OperationState*, ReplyEntry> m_replies;

public:
    explicit QtNetworkWorker(QObject* parent = nullptr): QObject(parent) {}

    ~QtNetworkWorker() override { shutdown(); }

    void ensure_manager() {
        if (m_manager == nullptr) {
            m_manager = new QNetworkAccessManager(this);
            m_router  = new QtNetworkManagerRouter(m_manager, true, this);
        }
    }

    void start(Arc<OperationState> state, rstd::Option<rstd::bytes::Bytes> body) {
        ensure_manager();

        auto manager_result = m_router->manager_for(state->request);
        if (manager_result.is_err()) {
            fail_start(rstd::move(state), rstd::move(manager_result).unwrap_err());
            return;
        }
        auto* manager = rstd::move(manager_result).unwrap();

        if (state->proxy.is_some()) apply_proxy(manager, *state->proxy);

        auto request = make_qnetwork_request(state->request);
        if (request.is_err()) {
            fail_start(rstd::move(state), rstd::move(request).unwrap_err());
            return;
        }

        auto* reply =
            send_request(manager, rstd::move(request).unwrap(), state->operation, rstd::move(body));

        if (reply == nullptr) {
            fail_start(rstd::move(state), Error::InvalidState("Qt did not create a reply"));
            return;
        }

        (void)m_replies.insert(state.as_ptr(),
                               ReplyEntry { QPointer<QNetworkReply>(reply), state.clone() });
        connect_reply(state, reply);
        state->complete_ready();
    }

    void cancel(OperationState* state) {
        auto entry = m_replies.get(state);
        if (entry.is_none()) return;

        auto* reply = (**entry).reply.data();
        if (reply != nullptr && ! reply->isFinished()) {
            reply->abort();
        }
    }

    void shutdown() {
        auto entries = m_replies.iter_mut();
        for (auto next = entries.next(); next.is_some(); next = entries.next()) {
            auto entry = next->template get<1>();
            auto state = entry->state.clone();
            if (! state->finished.exchange(true)) {
                state->complete_ready(Some(Error::Canceled()));
                state->push_body_event(BodyEvent::Failed(Error::Canceled()));
            }

            auto* reply = entry->reply.data();
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
        }
        m_replies.clear();
    }

private:
    void fail_start(Arc<OperationState> state, Error error) {
        state->finished.store(true);
        state->complete_ready(Some(rstd::move(error)));
        state->close_body();
    }

    void connect_reply(const Arc<OperationState>& state, QNetworkReply* reply) {
        QObject::connect(reply,
                         &QNetworkReply::metaDataChanged,
                         reply,
                         [this, capture = make_clone_tuple(state.clone()), reply] {
                             emit_header(capture.get<0>(), reply);
                         });
        QObject::connect(reply,
                         &QNetworkReply::readyRead,
                         reply,
                         [this, capture = make_clone_tuple(state.clone()), reply] {
                             emit_chunks(capture.get<0>(), reply);
                         });
        QObject::connect(reply,
                         &QIODevice::readChannelFinished,
                         reply,
                         [this, capture = make_clone_tuple(state.clone()), reply] {
                             emit_chunks(capture.get<0>(), reply);
                         });
        QObject::connect(reply,
                         &QNetworkReply::finished,
                         reply,
                         [this, capture = make_clone_tuple(state.clone()), reply] {
                             finish_reply(capture.get<0>(), reply);
                         });
        QObject::connect(
            reply, &QObject::destroyed, this, [this, capture = make_clone_tuple(state.clone())] {
                auto& operation = capture.get<0>();
                if (operation->finished.load()) return;
                operation->finished.store(true);
                operation->push_body_event(
                    BodyEvent::Failed(Error::InvalidState("QNetworkReply was destroyed")));
                (void)m_replies.remove(operation.as_ptr());
            });
    }

    void emit_header(const Arc<OperationState>& state, QNetworkReply* reply) {
        publish_header(state, reply);
    }

    void emit_chunks(const Arc<OperationState>& state, QNetworkReply* reply) {
        publish_chunks(state, reply);
    }

    void finish_reply(const Arc<OperationState>& state, QNetworkReply* reply) {
        publish_finished(state, reply);
        (void)m_replies.remove(state.as_ptr());
    }
};

class QtNetworkDriver : public NoCopy {
    QThread          m_thread;
    QtNetworkWorker* m_worker { nullptr };

public:
    QtNetworkDriver() {
        m_worker = new QtNetworkWorker();
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }

    ~QtNetworkDriver() {
        if (m_worker != nullptr && m_thread.isRunning()) {
            (void)QMetaObject::invokeMethod(
                m_worker,
                [worker = m_worker] {
                    worker->shutdown();
                },
                Qt::BlockingQueuedConnection);
            m_thread.quit();
            m_thread.wait();
        }
    }

    auto start(const Arc<OperationState>& state, rstd::Option<rstd::bytes::Bytes> body) -> bool {
        if (m_worker == nullptr || ! m_thread.isRunning()) {
            return false;
        }

        auto body_state = Arc<rstd::Option<rstd::bytes::Bytes>>::make(rstd::move(body));
        return QMetaObject::invokeMethod(
            m_worker,
            [worker  = m_worker,
             capture = make_clone_tuple(state.clone(), rstd::move(body_state))]() mutable {
                worker->start(rstd::move(capture.get<0>()), rstd::move(*capture.get<1>()));
            },
            Qt::QueuedConnection);
    }

    void cancel(OperationState* state) {
        if (m_worker == nullptr || ! m_thread.isRunning()) {
            return;
        }

        (void)QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, state] {
                worker->cancel(state);
            },
            Qt::QueuedConnection);
    }
};

void OperationState::cancel() {
    if (cancel_requested.exchange(true)) return;

    if (auto locked = driver.upgrade()) {
        locked->cancel(this);
        return;
    }

    if (executor.is_some() && direct.is_some()) {
        auto state = direct->clone();
        (void)executor->post([state = rstd::move(state)] {
            auto* reply = state->reply.data();
            if (reply != nullptr && ! reply->isFinished()) {
                reply->abort();
            }
        });
    }
}

export class SessionBackend : public NoCopy {
public:
    SessionBackend(): m_driver(Some(Arc<QtNetworkDriver>::make())) {}

    explicit SessionBackend(QObject* parent)
        : m_driver(parent == nullptr ? Some(Arc<QtNetworkDriver>::make())
                                     : None<Arc<QtNetworkDriver>>()),
          m_manager(parent == nullptr ? nullptr : new QNetworkAccessManager(parent)),
          m_executor(make_qt_executor(m_manager.data())) {
        if (m_manager != nullptr) {
            m_router = new QtNetworkManagerRouter(m_manager, true);
        }
    }

    explicit SessionBackend(QNetworkAccessManager* manager)
        : m_manager(manager), m_executor(make_qt_executor(manager)) {
        if (m_manager != nullptr) {
            m_router = new QtNetworkManagerRouter(m_manager, false);
        }
    }

    ~SessionBackend();

    template<typename... Args>
    static auto make(Args&&... args) -> Arc<SessionBackend> {
        return Arc<SessionBackend>::make(rstd::forward<Args>(args)...);
    }

    auto start_request(const Request& req, http::Operation operation,
                       rstd::Option<rstd::bytes::Bytes> body) -> coro<Result<ResponseBackend>>;

    auto get(const Request& req) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request& req) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request& req, rstd::bytes::Bytes body) -> coro<Result<Arc<ResponseBackend>>>;

    void set_proxy(const req_opt::Proxy&);
    void set_verify_certificate(bool);

private:
    auto prepare_req(const Request&) const -> Request;
    auto start_request_direct(const Request&, http::Operation, rstd::Option<rstd::bytes::Bytes>)
        -> coro<Result<ResponseBackend>>;

private:
    Option<Arc<QtNetworkDriver>>           m_driver;
    QPointer<QNetworkAccessManager>        m_manager;
    QPointer<QtNetworkManagerRouter>       m_router;
    rstd::Option<rstd::async::AnyExecutor> m_executor;
    rstd::Option<req_opt::Proxy>           m_proxy;
    bool                                   m_verify_certificate { true };
};

export class ResponseBackend : public NoCopy {
    friend class SessionBackend;

public:
    static constexpr usize ReadSize { 1024 * 16 };

    ~ResponseBackend() noexcept { cancel(); }
    ResponseBackend(ResponseBackend&& other) noexcept
        : m_req(rstd::move(other.m_req)),
          m_operation(other.m_operation),
          m_state(rstd::move(other.m_state)),
          m_body(other.m_body.take()),
          m_header(other.m_header.take()) {}

    auto operator=(ResponseBackend&& other) noexcept -> ResponseBackend& {
        if (this == &other) return *this;

        cancel();
        m_req       = rstd::move(other.m_req);
        m_operation = other.m_operation;
        m_state     = rstd::move(other.m_state);
        m_body      = other.m_body.take();
        m_header    = other.m_header.take();
        return *this;
    }

    auto header() const -> const http::Header& {
        return m_header.is_some() ? m_header->headers() : m_empty_header;
    }
    auto head() const -> rstd::Option<rstd::ref<http::MessageHead>> {
        if (m_header.is_none()) return None<rstd::ref<http::MessageHead>>();
        return Some(rstd::ref<http::MessageHead>::from_raw_parts(&*m_header));
    }
    auto trailers() const -> rstd::Option<rstd::ref<http::Header>> {
        return None<rstd::ref<http::Header>>();
    }
    auto code() const -> rstd::Option<i32> {
        if (m_header.is_none()) return None<i32>();
        auto status = m_header->status_code();
        if (status.is_none()) return None<i32>();
        return Some<i32>(rstd::as_cast<i32>(*status));
    }

    auto bytes() -> coro<Result<rstd::bytes::Bytes>>;
    auto is_finished() const -> bool { return ! m_state || m_state->finished.load(); }
    auto request() const -> const Request& { return m_req; }
    auto operation() const -> http::Operation { return m_operation; }

    void cancel() {
        if (m_state) m_state->cancel();
    }

private:
    ResponseBackend(Arc<OperationState> state, rstd::async::CompletionQueue<BodyEvent> body)
        : m_req(state->request.clone()),
          m_operation(state->operation),
          m_state(rstd::move(state)),
          m_body(Some(rstd::move(body))) {}

    Request                                               m_req;
    http::Operation                                       m_operation;
    Arc<OperationState>                                   m_state;
    rstd::Option<rstd::async::CompletionQueue<BodyEvent>> m_body;
    rstd::Option<http::MessageHead>                       m_header;
    http::Header                                          m_empty_header;
};

auto SessionBackend::prepare_req(const Request& req) const -> Request {
    auto out = req.clone();
    if (m_proxy) out.set_opt(m_proxy.clone().unwrap());
    out.get_opt<req_opt::SSL>().verify_certificate = m_verify_certificate;
    return out;
}

SessionBackend::~SessionBackend() {
    auto* router = m_router.data();
    if (router == nullptr) return;
    if (router->thread() == QThread::currentThread()) {
        delete router;
    } else {
        (void)QMetaObject::invokeMethod(router, &QObject::deleteLater, Qt::QueuedConnection);
    }
}

auto SessionBackend::start_request_direct(const Request& req, http::Operation operation,
                                          rstd::Option<rstd::bytes::Bytes> body)
    -> coro<Result<ResponseBackend>> {
    if (m_executor.is_none()) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt executor is unavailable")));
    }

    auto ready_completion = rstd::async::Completion<rstd::Option<Error>>::make();
    if (ready_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(ready_completion).unwrap_err_unchecked())));
    }
    auto ready_pair = rstd::move(ready_completion).unwrap_unchecked();

    auto body_completion = rstd::async::CompletionQueue<BodyEvent>::make();
    if (body_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(body_completion).unwrap_err_unchecked())));
    }
    auto body_pair = rstd::move(body_completion).unwrap_unchecked();

    if (! co_await m_executor->clone()) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt executor rejected request")));
    }

    auto  prepared = prepare_req(req);
    auto* router   = m_router.data();
    if (router == nullptr) {
        co_return Result<ResponseBackend>(
            Err(Error::InvalidState("Qt manager router is unavailable")));
    }

    auto manager_result = router->manager_for(prepared);
    if (manager_result.is_err()) {
        co_return Result<ResponseBackend>(Err(rstd::move(manager_result).unwrap_err()));
    }
    auto* manager = rstd::move(manager_result).unwrap();
    if (manager->thread() != QThread::currentThread()) {
        co_return Result<ResponseBackend>(
            Err(Error::InvalidState("QNetworkAccessManager must be used from its owner thread")));
    }

    auto request = make_qnetwork_request(prepared);
    if (request.is_err()) {
        co_return Result<ResponseBackend>(Err(rstd::move(request).unwrap_err()));
    }

    auto proxy = rstd::Option<req_opt::Proxy> {};
    if (m_proxy) {
        proxy = Some(m_proxy.clone().unwrap());
        apply_proxy(manager, *proxy);
    }

    auto state = Arc<OperationState>::make(rstd::move(prepared),
                                           operation,
                                           Weak<QtNetworkDriver>::make(),
                                           Some(m_executor->clone()),
                                           rstd::move(proxy),
                                           rstd::move(ready_pair.get<1>()),
                                           rstd::move(body_pair.get<1>()));

    auto* reply = send_request(manager, rstd::move(request).unwrap(), operation, rstd::move(body));

    if (reply == nullptr) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt did not create a reply")));
    }

    state->direct = Some(Arc<DirectReplyState>::make(DirectReplyState { reply }));
    QObject::connect(reply,
                     &QNetworkReply::metaDataChanged,
                     reply,
                     [capture = make_clone_tuple(state.clone()), reply] {
                         publish_header(capture.get<0>(), reply);
                     });
    QObject::connect(reply,
                     &QNetworkReply::readyRead,
                     reply,
                     [capture = make_clone_tuple(state.clone()), reply] {
                         publish_chunks(capture.get<0>(), reply);
                     });
    QObject::connect(reply,
                     &QIODevice::readChannelFinished,
                     reply,
                     [capture = make_clone_tuple(state.clone()), reply] {
                         publish_chunks(capture.get<0>(), reply);
                     });
    QObject::connect(
        reply, &QNetworkReply::finished, reply, [capture = make_clone_tuple(state.clone()), reply] {
            publish_finished(capture.get<0>(), reply);
            reply->deleteLater();
        });
    QObject::connect(
        reply, &QObject::destroyed, manager, [capture = make_clone_tuple(state.clone())] {
            auto& operation = capture.get<0>();
            if (operation->finished.load()) return;
            operation->finished.store(true);
            operation->push_body_event(
                BodyEvent::Failed(Error::InvalidState("QNetworkReply was destroyed")));
        });
    state->complete_ready();

    auto ready = co_await rstd::move(ready_pair.get<0>());
    if (ready.is_err()) {
        co_return Result<ResponseBackend>(Err(Error::Canceled()));
    }
    auto ready_error = rstd::move(ready).unwrap_unchecked();
    if (ready_error.is_some()) {
        co_return Result<ResponseBackend>(Err(rstd::move(ready_error).unwrap_unchecked()));
    }

    co_return Result<ResponseBackend>(
        Ok(ResponseBackend(rstd::move(state), rstd::move(body_pair.get<0>()))));
}

auto SessionBackend::start_request(const Request& req, http::Operation operation,
                                   rstd::Option<rstd::bytes::Bytes> body)
    -> coro<Result<ResponseBackend>> {
    if (m_driver.is_none()) {
        co_return co_await start_request_direct(req, operation, rstd::move(body));
    }

    auto prepared = prepare_req(req);

    auto proxy = rstd::Option<req_opt::Proxy> {};
    if (m_proxy) {
        proxy = Some(m_proxy.clone().unwrap());
    }

    auto ready_completion = rstd::async::Completion<rstd::Option<Error>>::make();
    if (ready_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(ready_completion).unwrap_err_unchecked())));
    }
    auto ready_pair = rstd::move(ready_completion).unwrap_unchecked();

    auto body_completion = rstd::async::CompletionQueue<BodyEvent>::make();
    if (body_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(body_completion).unwrap_err_unchecked())));
    }
    auto body_pair = rstd::move(body_completion).unwrap_unchecked();

    auto state = Arc<OperationState>::make(rstd::move(prepared),
                                           operation,
                                           m_driver->downgrade(),
                                           None<rstd::async::AnyExecutor>(),
                                           rstd::move(proxy),
                                           rstd::move(ready_pair.get<1>()),
                                           rstd::move(body_pair.get<1>()));

    if (! (*m_driver)->start(state, rstd::move(body))) {
        state->close_body();
        co_return Result<ResponseBackend>(
            Err(Error::InvalidState("Qt network driver is not running")));
    }

    auto ready = co_await rstd::move(ready_pair.get<0>());
    if (ready.is_err()) {
        co_return Result<ResponseBackend>(Err(Error::Canceled()));
    }
    auto ready_error = rstd::move(ready).unwrap_unchecked();
    if (ready_error.is_some()) {
        co_return Result<ResponseBackend>(Err(rstd::move(ready_error).unwrap_unchecked()));
    }

    co_return Result<ResponseBackend>(
        Ok(ResponseBackend(rstd::move(state), rstd::move(body_pair.get<0>()))));
}

auto SessionBackend::get(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    auto res = co_await start_request(req, http::Operation::Get(), None<rstd::bytes::Bytes>());
    if (res.is_err()) {
        co_return Result<Arc<ResponseBackend>>(Err(rstd::move(res).unwrap_err()));
    }
    co_return Result<Arc<ResponseBackend>>(
        Ok(Arc<ResponseBackend>::make(rstd::move(res).unwrap())));
}

auto SessionBackend::post(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    co_return co_await post(req, rstd::bytes::Bytes::make());
}

auto SessionBackend::post(const Request& req, rstd::bytes::Bytes body)
    -> coro<Result<Arc<ResponseBackend>>> {
    auto res = co_await start_request(req, http::Operation::Post(), Some(rstd::move(body)));
    if (res.is_err()) {
        co_return Result<Arc<ResponseBackend>>(Err(rstd::move(res).unwrap_err()));
    }
    co_return Result<Arc<ResponseBackend>>(
        Ok(Arc<ResponseBackend>::make(rstd::move(res).unwrap())));
}

void SessionBackend::set_proxy(const req_opt::Proxy& proxy) { m_proxy = Some(proxy.clone()); }

void SessionBackend::set_verify_certificate(bool value) { m_verify_certificate = value; }

auto ResponseBackend::bytes() -> coro<Result<rstd::bytes::Bytes>> {
    rstd::bytes::BytesMut out = rstd::bytes::BytesMut::with_capacity(ReadSize);

    if (m_body.is_none()) {
        co_return Result<rstd::bytes::Bytes>(
            Err(Error::InvalidState("Qt response body queue is unavailable")));
    }

    for (;;) {
        auto next = co_await m_body->next();
        if (next.is_err()) {
            co_return Result<rstd::bytes::Bytes>(
                Err(Error::Io(rstd::move(next).unwrap_err_unchecked())));
        }
        auto item = rstd::move(next).unwrap_unchecked();
        if (item.is_none()) {
            co_return Result<rstd::bytes::Bytes>(
                Err(Error::InvalidState("Qt response body ended without finished event")));
        }

        auto event = rstd::move(item).unwrap_unchecked();
        if (event.is_Header()) {
            m_header = Some(rstd::move(event).as_Header().value);
            continue;
        }
        if (event.is_Chunk()) {
            auto chunk = rstd::move(event).as_Chunk().value;
            out.extend_from_slice(chunk.as_slice());
            continue;
        }
        if (event.is_Finished()) {
            co_return Result<rstd::bytes::Bytes>(Ok(out.freeze()));
        }
        co_return Result<rstd::bytes::Bytes>(Err(rstd::move(event).as_Failed().value));
    }
}

} // namespace ncrequest::client::qt_network
