module;
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <QByteArray>
#include <QBuffer>
#include <QCoreApplication>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QThread>
#include <QUrl>

export module ncrequest:client_qt_network;
export import :request;
export import :http;
export import :error;
export import ncrequest.coro;
export import ncrequest.type;

namespace ncrequest::client::qt_network
{

export struct Options {};

export class ResponseBackend;
class QtNetworkDriver;

class QtExecutor {
    QPointer<QObject> m_target;

public:
    explicit QtExecutor(QObject* target): m_target(target) {}

    auto post_job(rstd::async::ExecutorJob job) -> bool {
        auto* target = m_target.data();
        if (target == nullptr) return false;
        auto owned = std::make_shared<rstd::async::ExecutorJob>(rstd::move(job));
        return QMetaObject::invokeMethod(
            target,
            [owned = rstd::move(owned)]() mutable {
                owned->run();
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

auto make_qt_executor(QObject* target) -> rstd::Option<rstd::async::AnyExecutor> {
    if (target == nullptr) return None<rstd::async::AnyExecutor>();
    return Some(rstd::async::AnyExecutor::from_executor(QtExecutor { target }));
}

struct BodyEvent {
    enum class Kind
    {
        Header,
        Chunk,
        Finished,
        Error,
    };

    Kind                             kind { Kind::Finished };
    rstd::Option<HttpHeader>         header;
    rstd::Option<rstd::bytes::Bytes> chunk;
    rstd::Option<Error>              error;

    static auto make_header(HttpHeader header) -> BodyEvent {
        auto out   = BodyEvent {};
        out.kind   = Kind::Header;
        out.header = Some(rstd::move(header));
        return out;
    }

    static auto make_chunk(rstd::bytes::Bytes chunk) -> BodyEvent {
        auto out  = BodyEvent {};
        out.kind  = Kind::Chunk;
        out.chunk = Some(rstd::move(chunk));
        return out;
    }

    static auto make_finished() -> BodyEvent {
        auto out = BodyEvent {};
        out.kind = Kind::Finished;
        return out;
    }

    static auto make_error(Error error) -> BodyEvent {
        auto out  = BodyEvent {};
        out.kind  = Kind::Error;
        out.error = Some(rstd::move(error));
        return out;
    }
};

struct DirectReplyState {
    QPointer<QNetworkReply> reply;
};

struct OperationState {
    Request                                            request;
    Operation                                          operation;
    Weak<QtNetworkDriver>                              driver;
    rstd::Option<rstd::async::AnyExecutor>             executor;
    std::shared_ptr<DirectReplyState>                  direct;
    rstd::Option<req_opt::Proxy>                       proxy;
    rstd::async::CompletionHandle<rstd::Option<Error>> ready;
    rstd::async::CompletionQueueHandle<BodyEvent>      body;
    std::atomic<bool>                                  finished { false };
    std::atomic<bool>                                  cancel_requested { false };

    OperationState(Request request, Operation operation, Weak<QtNetworkDriver> driver,
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
        auto close =
            event.kind == BodyEvent::Kind::Finished || event.kind == BodyEvent::Kind::Error;
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

    for (auto const& [name, value] : req.header()) {
        request.setRawHeader(QByteArray(name.data(), static_cast<qsizetype>(name.size())),
                             QByteArray(value.data(), static_cast<qsizetype>(value.size())));
    }

    auto const& timeout = req.get_opt<req_opt::Timeout>();
    if (timeout.transfer_timeout > 0) {
        request.setTransferTimeout(static_cast<int>(timeout.transfer_timeout));
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

auto read_header(QNetworkReply* reply) -> HttpHeader {
    auto header = HttpHeader {};
    if (reply == nullptr) return header;

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) {
        header.start = Some<HttpHeader::Start>(
            HttpHeader::Status { .version = "HTTP", .code = status.toInt() });
    }

    for (auto const& pair : reply->rawHeaderPairs()) {
        header.fields.push_back(HttpHeader::Field {
            .name  = std::string(pair.first.constData(), static_cast<usize>(pair.first.size())),
            .value = std::string(pair.second.constData(), static_cast<usize>(pair.second.size())),
        });
    }

    return header;
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
    state->push_body_event(BodyEvent::make_header(read_header(reply)));
}

void publish_chunks(const Arc<OperationState>& state, QNetworkReply* reply) {
    if (state->finished.load() || reply == nullptr) return;

    while (reply->bytesAvailable() > 0) {
        auto chunk = reply->read(reply->bytesAvailable());
        if (chunk.isEmpty()) break;

        auto bytes = rstd::bytes::Bytes::copy_from_slice(rstd::slice<rstd::u8>::from_raw_parts(
            reinterpret_cast<const rstd::u8*>(chunk.constData()),
            static_cast<rstd::usize>(chunk.size())));
        state->push_body_event(BodyEvent::make_chunk(rstd::move(bytes)));
    }
}

void publish_finished(const Arc<OperationState>& state, QNetworkReply* reply) {
    if (state->finished.load()) return;

    publish_header(state, reply);
    publish_chunks(state, reply);
    state->finished.store(true);

    auto error = transport_error(reply);
    if (error.is_some()) {
        state->push_body_event(BodyEvent::make_error(rstd::move(error).unwrap()));
    } else {
        state->push_body_event(BodyEvent::make_finished());
    }
}

class QtNetworkWorker : public QObject {
    struct ReplyEntry {
        QPointer<QNetworkReply> reply;
        Arc<OperationState>     state;
    };

    QNetworkAccessManager*                          m_manager { nullptr };
    std::unordered_map<OperationState*, ReplyEntry> m_replies;

public:
    explicit QtNetworkWorker(QObject* parent = nullptr): QObject(parent) {}

    ~QtNetworkWorker() override { shutdown(); }

    void ensure_manager() {
        if (m_manager == nullptr) {
            m_manager = new QNetworkAccessManager(this);
        }
    }

    void start(Arc<OperationState> state, rstd::Option<rstd::bytes::Bytes> body) {
        ensure_manager();

        if (state->proxy.is_some()) {
            apply_proxy(m_manager, *state->proxy);
        }

        auto request = make_qnetwork_request(state->request);
        if (request.is_err()) {
            fail_start(rstd::move(state), rstd::move(request).unwrap_err());
            return;
        }

        QNetworkReply* reply = nullptr;
        if (state->operation == Operation::PostOperation) {
            auto qrequest = rstd::move(request).unwrap();
            if (! qrequest.hasRawHeader("Content-Type")) {
                qrequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
            }

            QByteArray payload;
            if (body.is_some()) {
                auto bytes = rstd::move(body).unwrap();
                payload    = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                        static_cast<qsizetype>(bytes.size()));
            }
            auto* upload = new QBuffer(this);
            upload->setData(payload);
            upload->open(QIODevice::ReadOnly);
            reply = m_manager->post(qrequest, upload);
            if (reply != nullptr) {
                upload->setParent(reply);
            } else {
                delete upload;
            }
        } else {
            reply = m_manager->get(rstd::move(request).unwrap());
        }

        if (reply == nullptr) {
            fail_start(rstd::move(state), Error::InvalidState("Qt did not create a reply"));
            return;
        }

        m_replies[state.get()] = ReplyEntry { QPointer<QNetworkReply>(reply), state };
        connect_reply(state, reply);
        state->complete_ready();
    }

    void cancel(OperationState* state) {
        auto it = m_replies.find(state);
        if (it == m_replies.end()) return;

        auto* reply = it->second.reply.data();
        if (reply != nullptr && ! reply->isFinished()) {
            reply->abort();
        }
    }

    void shutdown() {
        for (auto& [_, entry] : m_replies) {
            auto state = entry.state;
            if (! state->finished.exchange(true)) {
                state->complete_ready(Some(Error::Canceled()));
                state->push_body_event(BodyEvent::make_error(Error::Canceled()));
            }

            auto* reply = entry.reply.data();
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

    void connect_reply(Arc<OperationState> state, QNetworkReply* reply) {
        QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [this, state, reply] {
            emit_header(state, reply);
        });
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [this, state, reply] {
            emit_chunks(state, reply);
        });
        QObject::connect(reply, &QIODevice::readChannelFinished, reply, [this, state, reply] {
            emit_chunks(state, reply);
        });
        QObject::connect(reply, &QNetworkReply::finished, reply, [this, state, reply] {
            finish_reply(state, reply);
        });
        QObject::connect(reply, &QObject::destroyed, this, [this, state] {
            if (state->finished.load()) return;
            state->finished.store(true);
            state->push_body_event(
                BodyEvent::make_error(Error::InvalidState("QNetworkReply was destroyed")));
            m_replies.erase(state.get());
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
        m_replies.erase(state.get());
    }
};

class QtNetworkDriver : public std::enable_shared_from_this<QtNetworkDriver>, public NoCopy {
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

    auto start(Arc<OperationState> state, rstd::Option<rstd::bytes::Bytes> body) -> bool {
        if (m_worker == nullptr || ! m_thread.isRunning()) {
            return false;
        }

        auto body_state = std::make_shared<rstd::Option<rstd::bytes::Bytes>>(rstd::move(body));
        return QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, state = rstd::move(state), body_state]() mutable {
                worker->start(rstd::move(state), rstd::move(*body_state));
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

    if (auto locked = driver.lock()) {
        locked->cancel(this);
        return;
    }

    if (executor.is_some() && direct) {
        auto state = direct;
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
    SessionBackend(): m_driver(std::make_shared<QtNetworkDriver>()) {}

    explicit SessionBackend(QObject* parent)
        : m_driver(parent == nullptr ? std::make_shared<QtNetworkDriver>() : nullptr),
          m_manager(parent == nullptr ? nullptr : new QNetworkAccessManager(parent)),
          m_executor(make_qt_executor(m_manager.data())) {}

    explicit SessionBackend(QNetworkAccessManager* manager)
        : m_manager(manager), m_executor(make_qt_executor(manager)) {}

    ~SessionBackend() = default;

    template<typename... Args>
    static auto make(Args&&... args) -> Arc<SessionBackend> {
        return make_arc<SessionBackend>(rstd::forward<Args>(args)...);
    }

    auto start_request(const Request& req, Operation operation,
                       rstd::Option<rstd::bytes::Bytes> body) -> coro<Result<ResponseBackend>>;

    auto get(const Request& req) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request& req) -> coro<Result<Arc<ResponseBackend>>>;
    auto post(const Request& req, rstd::bytes::Bytes body) -> coro<Result<Arc<ResponseBackend>>>;

    void set_proxy(const req_opt::Proxy&);
    void set_verify_certificate(bool);

private:
    auto manager() const -> QNetworkAccessManager* { return m_manager.data(); }
    auto prepare_req(const Request&) const -> Request;
    auto to_qnetwork_request(const Request&) const -> Result<QNetworkRequest>;
    auto start_request_direct(const Request&, Operation, rstd::Option<rstd::bytes::Bytes>)
        -> coro<Result<ResponseBackend>>;

private:
    std::shared_ptr<QtNetworkDriver>       m_driver;
    QPointer<QNetworkAccessManager>        m_manager;
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
          m_header(rstd::move(other.m_header)) {}

    auto operator=(ResponseBackend&& other) noexcept -> ResponseBackend& {
        if (this == &other) return *this;

        cancel();
        m_req       = rstd::move(other.m_req);
        m_operation = other.m_operation;
        m_state     = rstd::move(other.m_state);
        m_body      = other.m_body.take();
        m_header    = rstd::move(other.m_header);
        return *this;
    }

    auto header() const -> const HttpHeader& { return m_header; }
    auto code() const -> rstd::Option<i32> {
        if (m_header.start.is_some()) {
            if (auto* start = std::get_if<HttpHeader::Status>(&*m_header.start)) {
                return Some<i32>(start->code);
            }
        }

        return None<i32>();
    }

    auto bytes() -> coro<Result<rstd::bytes::Bytes>>;
    auto text() -> coro<Result<std::string>>;

    auto is_finished() const -> bool { return ! m_state || m_state->finished.load(); }
    auto request() const -> const Request& { return m_req; }
    auto operation() const -> Operation { return m_operation; }

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
    Operation                                             m_operation;
    Arc<OperationState>                                   m_state;
    rstd::Option<rstd::async::CompletionQueue<BodyEvent>> m_body;
    HttpHeader                                            m_header;
};

auto SessionBackend::prepare_req(const Request& req) const -> Request {
    auto out = req.clone();
    if (m_proxy) out.set_opt(m_proxy.clone().unwrap());
    out.get_opt<req_opt::SSL>().verify_certificate = m_verify_certificate;
    return out;
}

auto SessionBackend::to_qnetwork_request(const Request& req) const -> Result<QNetworkRequest> {
    auto* manager = this->manager();
    if (manager == nullptr) {
        return Err(Error::InvalidState("QNetworkAccessManager is not available"));
    }
    if (manager->thread() != QThread::currentThread()) {
        return Err(Error::InvalidState("QNetworkAccessManager must be used from its owner thread"));
    }

    return make_qnetwork_request(req);
}

auto SessionBackend::start_request_direct(const Request& req, Operation operation,
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

    auto prepared = prepare_req(req);
    auto request  = to_qnetwork_request(prepared);
    if (request.is_err()) {
        co_return Result<ResponseBackend>(Err(rstd::move(request).unwrap_err()));
    }

    auto proxy = rstd::Option<req_opt::Proxy> {};
    if (m_proxy) {
        proxy = Some(m_proxy.clone().unwrap());
        apply_proxy(manager(), *proxy);
    }

    auto state = std::make_shared<OperationState>(rstd::move(prepared),
                                                  operation,
                                                  Weak<QtNetworkDriver> {},
                                                  Some(m_executor->clone()),
                                                  rstd::move(proxy),
                                                  rstd::move(ready_pair.get<1>()),
                                                  rstd::move(body_pair.get<1>()));

    auto*          manager = this->manager();
    QNetworkReply* reply   = nullptr;
    if (operation == Operation::PostOperation) {
        auto qrequest = rstd::move(request).unwrap();
        if (! qrequest.hasRawHeader("Content-Type")) {
            qrequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        }

        QByteArray payload;
        if (body.is_some()) {
            auto bytes = rstd::move(body).unwrap();
            payload    = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<qsizetype>(bytes.size()));
        }
        reply = manager->post(qrequest, payload);
    } else {
        reply = manager->get(rstd::move(request).unwrap());
    }

    if (reply == nullptr) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt did not create a reply")));
    }

    state->direct = std::make_shared<DirectReplyState>(DirectReplyState { reply });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [state, reply] {
        publish_header(state, reply);
    });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [state, reply] {
        publish_chunks(state, reply);
    });
    QObject::connect(reply, &QIODevice::readChannelFinished, reply, [state, reply] {
        publish_chunks(state, reply);
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [state, reply] {
        publish_finished(state, reply);
        reply->deleteLater();
    });
    QObject::connect(reply, &QObject::destroyed, manager, [state] {
        if (state->finished.load()) return;
        state->finished.store(true);
        state->push_body_event(
            BodyEvent::make_error(Error::InvalidState("QNetworkReply was destroyed")));
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

auto SessionBackend::start_request(const Request& req, Operation operation,
                                   rstd::Option<rstd::bytes::Bytes> body)
    -> coro<Result<ResponseBackend>> {
    if (! m_driver) {
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

    auto state = std::make_shared<OperationState>(rstd::move(prepared),
                                                  operation,
                                                  Weak<QtNetworkDriver>(m_driver),
                                                  None<rstd::async::AnyExecutor>(),
                                                  rstd::move(proxy),
                                                  rstd::move(ready_pair.get<1>()),
                                                  rstd::move(body_pair.get<1>()));

    if (! m_driver->start(state, rstd::move(body))) {
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
    auto res = co_await start_request(req, Operation::GetOperation, None<rstd::bytes::Bytes>());
    if (res.is_err()) {
        co_return Result<Arc<ResponseBackend>>(Err(rstd::move(res).unwrap_err()));
    }
    co_return Result<Arc<ResponseBackend>>(Ok(make_arc<ResponseBackend>(rstd::move(res).unwrap())));
}

auto SessionBackend::post(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    co_return co_await post(req, rstd::bytes::Bytes::make());
}

auto SessionBackend::post(const Request& req, rstd::bytes::Bytes body)
    -> coro<Result<Arc<ResponseBackend>>> {
    auto res = co_await start_request(req, Operation::PostOperation, Some(rstd::move(body)));
    if (res.is_err()) {
        co_return Result<Arc<ResponseBackend>>(Err(rstd::move(res).unwrap_err()));
    }
    co_return Result<Arc<ResponseBackend>>(Ok(make_arc<ResponseBackend>(rstd::move(res).unwrap())));
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
        switch (event.kind) {
        case BodyEvent::Kind::Header: {
            if (event.header.is_some()) {
                m_header = rstd::move(event.header).unwrap_unchecked();
            }
            break;
        }
        case BodyEvent::Kind::Chunk: {
            auto chunk = rstd::move(event.chunk).unwrap_unchecked();
            out.extend_from_slice(chunk.data(), chunk.size());
            break;
        }
        case BodyEvent::Kind::Finished: {
            co_return Result<rstd::bytes::Bytes>(Ok(out.freeze()));
        }
        case BodyEvent::Kind::Error: {
            co_return Result<rstd::bytes::Bytes>(Err(rstd::move(event.error).unwrap_unchecked()));
        }
        }
    }
}

auto ResponseBackend::text() -> coro<Result<std::string>> {
    auto data = co_await bytes();
    if (data.is_err()) {
        co_return Result<std::string>(Err(rstd::move(data).unwrap_err()));
    }

    auto        bytes = rstd::move(data).unwrap();
    std::string out;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    co_return Result<std::string>(Ok(rstd::move(out)));
}

} // namespace ncrequest::client::qt_network
