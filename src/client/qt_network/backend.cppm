module;
#include <rstd/enum.hpp>

export module ncrequest:client.qt_network.backend;
export import :client.qt_network.qt;
export import :request;
export import lihttpto;
export import :error;
export import ncrequest.coro;
export import rstd;
import :client.qt_network.session_share;

using namespace rstd::prelude;
using namespace ncrequest::qt;
using namespace rstd::literals;
using rstd::async::AnyExecutor;
using rstd::async::Completion;
using rstd::async::CompletionHandle;
using rstd::async::CompletionQueue;
using rstd::async::CompletionQueueHandle;
using rstd::async::ExecutorJob;
using rstd::bytes::Bytes;
using rstd::collections::HashMap;
using rstd::str_::from_utf8;
using rstd::sync::Arc;
using rstd::sync::Weak;
using rstd::sync::atomic::Atomic;

namespace ncrequest::client::qt_network
{

export struct Options {};

export class ResponseBackend;
class QtNetworkDriver;

// Qt copies queued and signal functors, so CloneTuple adapts explicit Arc cloning to that boundary.
class QtExecutor {
    QPointer<QObject> m_target;

public:
    explicit QtExecutor(QObject* target): m_target(target) {}

    auto post_job(ExecutorJob job) -> bool {
        auto* target = m_target.data();
        if (target == nullptr) return false;
        auto owned = Arc<ExecutorJob>::make(rstd::move(job));
        return QMetaObject::invokeMethod(
            target,
            [capture = make_clone_tuple(rstd::move(owned))]() mutable {
                capture.get<0>()->run();
            },
            Qt::QueuedConnection);
    }

    auto is_closed() -> bool { return m_target.isNull(); }
};

auto make_qt_executor(QObject* target) -> Option<AnyExecutor> {
    if (target == nullptr) return None<AnyExecutor>();
    return Some(AnyExecutor::from_executor(QtExecutor { target }));
}

struct BodyEvent {
    RSTD_ENUM(BodyEvent, (Header, (lihttpto::MessageHead value;)), (Chunk, (Bytes value;)),
              (Finished), (Failed, (Error value;)))
};

struct DirectReplyState {
    QPointer<QNetworkReply> reply;
};

struct OperationState {
    PreparedRequest                  request;
    Weak<QtNetworkDriver>            driver;
    Option<AnyExecutor>              executor;
    Option<Arc<DirectReplyState>>    direct;
    CompletionHandle<Option<Error>>  ready;
    CompletionQueueHandle<BodyEvent> body;
    Atomic<bool>                     finished { false };
    Atomic<bool>                     cancel_requested { false };

    OperationState(PreparedRequest request, Weak<QtNetworkDriver> driver,
                   Option<AnyExecutor> executor, CompletionHandle<Option<Error>> ready,
                   CompletionQueueHandle<BodyEvent> body)
        : request(rstd::move(request)),
          driver(rstd::move(driver)),
          executor(rstd::move(executor)),
          ready(rstd::move(ready)),
          body(rstd::move(body)) {}

    void cancel();

    void complete_ready(Option<Error> error = None<Error>()) {
        (void)ready.complete(rstd::move(error));
    }

    void push_body_event(BodyEvent event) {
        auto close = event.is_Finished() || event.is_Failed();
        (void)body.push(rstd::move(event));
        if (close) body.close();
    }

    void close_body() { body.close(); }
};

auto make_qnetwork_request(const PreparedRequest& source) -> Result<QNetworkRequest> {
    auto& req     = source.request();
    auto& options = source.options();
    if (QCoreApplication::instance() == nullptr) {
        return Err(Error::InvalidState("Qt network backend requires a QCoreApplication"));
    }

    auto            url = req.url();
    QNetworkRequest request { QUrl(
        QString::fromUtf8(reinterpret_cast<const char*>(url.data()), url.size().to_primitive())) };
    request.setAttribute(QNetworkRequest::AutoDeleteReplyOnFinishAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);

    auto headers = QHttpHeaders {};
    headers.reserve(static_cast<qsizetype>(req.header().len().to_primitive()));
    for (const auto& field : req.header()) {
        auto name  = field.name.as_str();
        auto value = field.value.as_slice();
        if (req.header().get_all(name).len() > usize(1)) {
            return Err(
                Error::Unsupported("Qt Network cannot preserve repeated request header fields"));
        }
        if (! headers.append(
                QLatin1StringView(reinterpret_cast<const char*>(name.data()),
                                  static_cast<qsizetype>(name.size().to_primitive())),
                QLatin1StringView(reinterpret_cast<const char*>(value.as_raw_ptr()),
                                  static_cast<qsizetype>(value.len().to_primitive())))) {
            return Err(Error::Protocol(ProtocolError::InvalidHeaderLine,
                                       protocol_error_message(ProtocolError::InvalidHeaderLine)));
        }
    }
    request.setHeaders(rstd::move(headers));

    auto const& tcp      = options.tcp();
    auto        defaults = TcpOptions {};
    if (tcp.keepalive != defaults.keepalive || tcp.keepidle != defaults.keepidle ||
        tcp.keepintvl != defaults.keepintvl)
        return Err(Error::Unsupported("Qt Network TCP keepalive options are not supported"));

    auto const& limits = options.limits();
    if (limits.header_bytes != ResourceLimits::DefaultHeaderBytes ||
        limits.receive_buffer_bytes != ResourceLimits::DefaultReceiveBufferBytes)
        return Err(Error::Unsupported(
            "Qt Network custom header and receive buffer limits are not supported"));

    auto const& timeout = options.timeout();
    if (timeout.connect.mode() != TimeoutLimit::Mode::BackendDefault)
        return Err(Error::Unsupported("Qt Network connect timeout policy is not supported"));
    if (timeout.total.mode() == TimeoutLimit::Mode::After)
        return Err(Error::Unsupported("Qt Network total timeout is not supported"));
    if (timeout.low_speed.is_some())
        return Err(Error::Unsupported("Qt Network low-speed timeout is not supported"));

    auto const& ssl = options.tls();
    if (! ssl.verify_peer || ! ssl.verify_hostname)
        return Err(Error::Unsupported(
            "Qt Network independent TLS verification policies are not supported"));
    if (ssl.ca_bundle.is_some() || ssl.client_identity.is_some())
        return Err(
            Error::Unsupported("Qt Network TLS CA bundle and client identity are not supported"));
    auto ssl_config = request.sslConfiguration();
    ssl_config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(ssl_config);

    return Ok(rstd::move(request));
}

class SystemProxyFactory final : public QNetworkProxyFactory {
public:
    auto queryProxy(const QNetworkProxyQuery& query) -> QList<QNetworkProxy> override {
        return QNetworkProxyFactory::systemProxyForQuery(query);
    }
};

auto apply_proxy(QNetworkAccessManager* manager, const ProxyOptions& proxy) -> Result<empty> {
    if (manager == nullptr) return Err(Error::InvalidState("Qt network manager is unavailable"));
    switch (proxy.mode()) {
    case ProxyOptions::Mode::System:
        manager->setProxyFactory(new SystemProxyFactory);
        return Ok(empty {});
    case ProxyOptions::Mode::Disabled:
        manager->setProxy(QNetworkProxy { QNetworkProxy::NoProxy });
        return Ok(empty {});
    case ProxyOptions::Mode::Explicit: break;
    }
    auto type = QNetworkProxy::HttpProxy;
    auto mode = proxy.type().unwrap();
    switch (mode) {
    case ProxyOptions::Type::HTTP: break;
    case ProxyOptions::Type::SOCKS5:
    case ProxyOptions::Type::SOCKS5H: type = QNetworkProxy::Socks5Proxy; break;
    default: return Err(Error::Unsupported("Qt Network does not support this proxy type"));
    }
    auto host = proxy.url().unwrap()->host().unwrap();
    if (host.starts_with("["_str)) host = *host.get(usize(1), host.size() - usize(1));
    auto value = QNetworkProxy { type,
                                 QString::fromUtf8(reinterpret_cast<const char*>(host.data()),
                                                   host.size().to_primitive()),
                                 static_cast<quint16>(proxy.port().unwrap().to_primitive()) };
    if (mode == ProxyOptions::Type::SOCKS5)
        value.setCapabilities(value.capabilities() & ~QNetworkProxy::HostNameLookupCapability);
    manager->setProxy(value);
    return Ok(empty {});
}

auto send_request(QNetworkAccessManager* manager, QNetworkRequest request, const Request& source)
    -> QNetworkReply* {
    if (manager == nullptr) return nullptr;
    auto method = source.method().as_ref();
    if (method == "HEAD"_str) return manager->head(request);
    auto payload = QByteArray {};
    if (source.body().bytes().is_some()) {
        auto raw = source.body().bytes()->as_slice();
        payload  = QByteArray(reinterpret_cast<const char*>(raw.as_raw_ptr()),
                              static_cast<qsizetype>(raw.len().to_primitive()));
    }
    if (method == "GET"_str && source.body().bytes().is_none()) return manager->get(request);
    if (method == "POST"_str) return manager->post(request, payload);
    auto raw  = method.as_bytes();
    auto verb = QByteArray(reinterpret_cast<const char*>(raw.as_raw_ptr()),
                           static_cast<qsizetype>(raw.len().to_primitive()));
    return manager->sendCustomRequest(request, verb, payload);
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

    auto manager_for(const PreparedRequest& request) -> Result<QNetworkAccessManager*> {
        auto* default_manager = m_default_manager.data();
        if (default_manager == nullptr) {
            return Err(Error::InvalidState("QNetworkAccessManager is not available"));
        }

        cleanup();
        auto const& share = request.options().share().share;
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
    -> rstd::Result<Option<lihttpto::MessageHead>, lihttpto::HttpParseError> {
    if (reply == nullptr) return Ok(None<lihttpto::MessageHead>());

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (! status.isValid()) return Ok(None<lihttpto::MessageHead>());

    auto status_code = lihttpto::StatusCode::make(static_cast<u16>(status.toInt()));
    if (status_code.is_err()) return Err(rstd::move(status_code).unwrap_err());

    auto headers = lihttpto::Headers {};

    for (auto const& pair : reply->headers().toListOfPairs()) {
        auto name_bytes =
            slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(pair.first.constData()),
                                      static_cast<usize>(pair.first.size()));
        auto name_text = from_utf8(name_bytes);
        if (name_text.is_err()) {
            return Err(lihttpto::HttpParseError {
                lihttpto::HttpParseErrorKind::InvalidHeaderLine(),
                name_text.unwrap_err().valid_up_to(),
            });
        }
        auto name = lihttpto::HeaderName::make(rstd::move(name_text).unwrap());
        if (name.is_err()) {
            return Err(lihttpto::HttpParseError { lihttpto::HttpParseErrorKind::InvalidHeaderLine(),
                                                  usize() });
        }

        auto value = lihttpto::HeaderValue::make(
            slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(pair.second.constData()),
                                      static_cast<usize>(pair.second.size())));
        if (value.is_err()) {
            return Err(lihttpto::HttpParseError { lihttpto::HttpParseErrorKind::InvalidHeaderLine(),
                                                  usize() });
        }
        headers.push(lihttpto::Header { rstd::move(name).unwrap(), rstd::move(value).unwrap() });
    }

    auto start =
        lihttpto::StartLine::Response(lihttpto::StatusLine { None<lihttpto::MessageVersion>(),
                                                             rstd::move(status_code).unwrap(),
                                                             None<lihttpto::HeaderValue>() });
    return Ok(Some(lihttpto::MessageHead { rstd::move(start), rstd::move(headers) }));
}

auto is_http_status_error(int status, QNetworkReply::NetworkError error) -> bool {
    switch (error) {
    case QNetworkReply::ProtocolInvalidOperationError: return status == 400 || status == 418;
    case QNetworkReply::AuthenticationRequiredError: return status == 401;
    case QNetworkReply::ContentAccessDenied: return status == 403;
    case QNetworkReply::ContentNotFoundError: return status == 404;
    case QNetworkReply::ContentOperationNotPermittedError: return status == 405;
    case QNetworkReply::ProxyAuthenticationRequiredError: return status == 407;
    case QNetworkReply::ContentConflictError: return status == 409;
    case QNetworkReply::ContentGoneError: return status == 410;
    case QNetworkReply::InternalServerError: return status == 500;
    case QNetworkReply::OperationNotImplementedError: return status == 501;
    case QNetworkReply::ServiceUnavailableError: return status == 503;
    case QNetworkReply::UnknownContentError: return status >= 400 && status < 500;
    case QNetworkReply::UnknownServerError: return status > 500;
    default: return false;
    }
}

auto transport_error(QNetworkReply* reply) -> Option<Error> {
    if (reply == nullptr) {
        return Some(Error::InvalidState("QNetworkReply was destroyed"));
    }

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    auto error  = reply->error();
    if (error == QNetworkReply::NoError) return None<Error>();
    if (error == QNetworkReply::OperationCanceledError) {
        return Some(Error::Canceled());
    }
    // Only suppress errors describing the HTTP status, never a failed transfer.
    if (status.isValid() && is_http_status_error(status.toInt(), error)) return None<Error>();

    auto utf8    = reply->errorString().toUtf8();
    auto message = String::make(
        from_utf8(slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(utf8.constData()),
                                            usize(utf8.size())))
            .unwrap());
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

        auto bytes = Bytes::copy_from_slice(slice<u8>::from_raw_parts(
            reinterpret_cast<const byte*>(chunk.constData()), static_cast<usize>(chunk.size())));
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

    void start(Arc<OperationState> state) {
        ensure_manager();

        auto manager_result = m_router->manager_for(state->request);
        if (manager_result.is_err()) {
            fail_start(rstd::move(state), rstd::move(manager_result).unwrap_err());
            return;
        }
        auto* manager = rstd::move(manager_result).unwrap();

        auto proxy = apply_proxy(manager, state->request.options().proxy());
        if (proxy.is_err()) {
            fail_start(rstd::move(state), rstd::move(proxy).unwrap_err());
            return;
        }

        auto request = make_qnetwork_request(state->request);
        if (request.is_err()) {
            fail_start(rstd::move(state), rstd::move(request).unwrap_err());
            return;
        }

        auto* reply = send_request(manager, rstd::move(request).unwrap(), state->request.request());

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

class QtNetworkDriver {
    QThread          m_thread;
    QtNetworkWorker* m_worker { nullptr };

public:
    QtNetworkDriver(const QtNetworkDriver&)                    = delete;
    auto operator=(const QtNetworkDriver&) -> QtNetworkDriver& = delete;

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

    auto start(const Arc<OperationState>& state) -> bool {
        if (m_worker == nullptr || ! m_thread.isRunning()) {
            return false;
        }

        return QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, capture = make_clone_tuple(state.clone())]() mutable {
                worker->start(rstd::move(capture.get<0>()));
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

export class SessionBackend {
public:
    SessionBackend(const SessionBackend&)                    = delete;
    auto operator=(const SessionBackend&) -> SessionBackend& = delete;

    static auto initialize() -> Result<empty> {
        if (QCoreApplication::instance() == nullptr)
            return Err(Error::InvalidState("Qt network backend requires a QCoreApplication"));
        return Ok(empty {});
    }
    static auto initialize(QObject* parent) -> Result<empty> {
        auto initialized = initialize();
        if (initialized.is_err()) return initialized;
        if (parent != nullptr && parent->thread() != QThread::currentThread())
            return Err(Error::InvalidState("Qt parent belongs to another thread"));
        return Ok(empty {});
    }
    static auto initialize(QNetworkAccessManager* manager) -> Result<empty> {
        if (manager == nullptr) return Err(Error::InvalidState("Qt network manager is null"));
        return initialize(static_cast<QObject*>(manager));
    }
    auto start() -> Result<empty> {
        if (m_driver.is_none() && m_manager == nullptr)
            return Err(Error::InvalidState("Qt network backend is unavailable"));
        return Ok(empty {});
    }

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
    void close();

    template<typename... Args>
    static auto make(Args&&... args) -> Result<Arc<SessionBackend>> {
        auto initialized = initialize(args...);
        if (initialized.is_err()) return Err(rstd::move(initialized).unwrap_err());
        auto backend = Arc<SessionBackend>::make(rstd::forward<Args>(args)...);
        auto started = backend->start();
        if (started.is_err()) return Err(rstd::move(started).unwrap_err());
        return Ok(rstd::move(backend));
    }

    auto start_request(PreparedRequest prepared) -> coro<Result<ResponseBackend>>;

private:
    auto start_request_direct(PreparedRequest) -> coro<Result<ResponseBackend>>;

private:
    Option<Arc<QtNetworkDriver>>     m_driver;
    QPointer<QNetworkAccessManager>  m_manager;
    QPointer<QtNetworkManagerRouter> m_router;
    Option<AnyExecutor>              m_executor;
};

export class ResponseBackend {
    friend class SessionBackend;

public:
    ResponseBackend(const ResponseBackend&)                    = delete;
    auto operator=(const ResponseBackend&) -> ResponseBackend& = delete;

    static constexpr usize ReadSize { 1024 * 16 };

    ~ResponseBackend() noexcept { cancel(); }
    ResponseBackend(ResponseBackend&& other) noexcept
        : m_state(rstd::move(other.m_state)),
          m_body(other.m_body.take()),
          m_header(other.m_header.take()) {}

    auto operator=(ResponseBackend&& other) noexcept -> ResponseBackend& {
        if (this == &other) return *this;

        cancel();
        m_state  = rstd::move(other.m_state);
        m_body   = other.m_body.take();
        m_header = other.m_header.take();
        return *this;
    }

    auto header() const -> const lihttpto::Headers& {
        return m_header.is_some() ? m_header->headers() : m_empty_header;
    }
    auto head() const -> Option<ref<lihttpto::MessageHead>> {
        if (m_header.is_none()) return None<ref<lihttpto::MessageHead>>();
        return Some(ref<lihttpto::MessageHead>::from_raw_parts(&*m_header));
    }
    auto trailers() const -> Option<ref<lihttpto::Headers>> {
        return None<ref<lihttpto::Headers>>();
    }
    auto code() const -> Option<i32> {
        if (m_header.is_none()) return None<i32>();
        auto status = m_header->status_code();
        if (status.is_none()) return None<i32>();
        return Some<i32>(as_cast<i32>(*status));
    }

    auto next_chunk() -> coro<Result<Option<Bytes>>>;
    auto ready_head() -> coro<Result<empty>>;
    auto is_finished() const -> bool { return ! m_state || m_state->finished.load(); }
    auto request() const -> const Request& { return m_state->request.request(); }

    void cancel() {
        if (m_state) m_state->cancel();
    }

private:
    ResponseBackend(Arc<OperationState> state, CompletionQueue<BodyEvent> body)
        : m_state(rstd::move(state)), m_body(Some(rstd::move(body))) {}

    Arc<OperationState>                m_state;
    Option<CompletionQueue<BodyEvent>> m_body;
    Option<lihttpto::MessageHead>      m_header;
    lihttpto::Headers                  m_empty_header;
};

SessionBackend::~SessionBackend() { close(); }

void SessionBackend::close() {
    m_driver     = None();
    auto* router = m_router.data();
    m_router     = nullptr;
    if (router == nullptr) return;
    if (router->thread() == QThread::currentThread()) {
        delete router;
    } else {
        (void)QMetaObject::invokeMethod(router, &QObject::deleteLater, Qt::QueuedConnection);
    }
}

auto SessionBackend::start_request_direct(PreparedRequest req) -> coro<Result<ResponseBackend>> {
    if (m_executor.is_none()) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt executor is unavailable")));
    }

    auto ready_completion = Completion<Option<Error>>::make();
    if (ready_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(ready_completion).unwrap_err_unchecked())));
    }
    auto ready_pair = rstd::move(ready_completion).unwrap_unchecked();

    auto body_completion = CompletionQueue<BodyEvent>::make();
    if (body_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(body_completion).unwrap_err_unchecked())));
    }
    auto body_pair = rstd::move(body_completion).unwrap_unchecked();

    if (! co_await m_executor->clone()) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt executor rejected request")));
    }

    auto  prepared = rstd::move(req);
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

    auto proxy = apply_proxy(manager, prepared.options().proxy());
    if (proxy.is_err()) co_return Err(rstd::move(proxy).unwrap_err());

    auto state = Arc<OperationState>::make(rstd::move(prepared),
                                           Weak<QtNetworkDriver>::make(),
                                           Some(m_executor->clone()),
                                           rstd::move(ready_pair.get<1>()),
                                           rstd::move(body_pair.get<1>()));

    auto* reply = send_request(manager, rstd::move(request).unwrap(), state->request.request());

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

auto SessionBackend::start_request(PreparedRequest prepared_request)
    -> coro<Result<ResponseBackend>> {
    if (prepared_request.options().endpoint().socket_path().is_some())
        co_return Err(Error::Unsupported("Qt Network does not support Unix socket endpoints"));
    auto req = rstd::move(prepared_request);
    if (req.request().body().stream().is_some())
        co_return Err(Error::Unsupported("Qt Network does not support asynchronous body sources"));
    if (req.request().body().reader().is_some())
        co_return Err(Error::Unsupported("Qt Network does not support read callbacks"));
    if (m_driver.is_none()) {
        co_return co_await start_request_direct(rstd::move(req));
    }

    auto prepared = rstd::move(req);

    auto ready_completion = Completion<Option<Error>>::make();
    if (ready_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(ready_completion).unwrap_err_unchecked())));
    }
    auto ready_pair = rstd::move(ready_completion).unwrap_unchecked();

    auto body_completion = CompletionQueue<BodyEvent>::make();
    if (body_completion.is_err()) {
        co_return Result<ResponseBackend>(
            Err(Error::Io(rstd::move(body_completion).unwrap_err_unchecked())));
    }
    auto body_pair = rstd::move(body_completion).unwrap_unchecked();

    auto state = Arc<OperationState>::make(rstd::move(prepared),
                                           m_driver->downgrade(),
                                           None<AnyExecutor>(),
                                           rstd::move(ready_pair.get<1>()),
                                           rstd::move(body_pair.get<1>()));

    if (! (*m_driver)->start(state)) {
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

auto ResponseBackend::ready_head() -> coro<Result<empty>> {
    if (m_header.is_some()) co_return Ok(empty {});
    if (m_body.is_none())
        co_return Err(Error::InvalidState("Qt response body queue is unavailable"));
    for (;;) {
        auto next = co_await m_body->next();
        if (next.is_err()) co_return Err(Error::Io(rstd::move(next).unwrap_err()));
        auto item = rstd::move(next).unwrap();
        if (item.is_none()) co_return Err(Error::Protocol(ProtocolError::UnexpectedEof, nullptr));
        auto event = rstd::move(item).unwrap();
        if (event.is_Failed()) co_return Err(rstd::move(event).as_Failed().value);
        if (! event.is_Header())
            co_return Err(Error::InvalidState("Qt response body arrived before headers"));
        m_header = Some(rstd::move(event).as_Header().value);
        co_return Ok(empty {});
    }
}

auto ResponseBackend::next_chunk() -> coro<Result<Option<Bytes>>> {
    if (m_body.is_none())
        co_return Err(Error::InvalidState("Qt response body queue is unavailable"));
    for (;;) {
        auto next = co_await m_body->next();
        if (next.is_err()) co_return Err(Error::Io(rstd::move(next).unwrap_err()));
        auto item = rstd::move(next).unwrap();
        if (item.is_none()) co_return Err(Error::Protocol(ProtocolError::UnexpectedEof, nullptr));
        auto event = rstd::move(item).unwrap();
        // Metadata notifications may repeat; the published response head is immutable.
        if (event.is_Header()) continue;
        if (event.is_Chunk()) co_return Ok(Some(rstd::move(event).as_Chunk().value));
        if (event.is_Finished()) co_return Ok(None<Bytes>());
        co_return Err(rstd::move(event).as_Failed().value);
    }
}

} // namespace ncrequest::client::qt_network
