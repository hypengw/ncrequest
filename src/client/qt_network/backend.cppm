module;
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <QByteArray>
#include <QBuffer>
#include <QCoreApplication>
#include <QList>
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

struct BodyEvent {
    enum class Kind {
        Header,
        Chunk,
        Finished,
        Error,
    };

    Kind                              kind { Kind::Finished };
    rstd::Option<HttpHeader>          header;
    rstd::Option<rstd::bytes::Bytes>  chunk;
    rstd::Option<Error>               error;

    static auto make_header(HttpHeader header) -> BodyEvent {
        auto out    = BodyEvent {};
        out.kind    = Kind::Header;
        out.header  = Some(rstd::move(header));
        return out;
    }

    static auto make_chunk(rstd::bytes::Bytes chunk) -> BodyEvent {
        auto out   = BodyEvent {};
        out.kind   = Kind::Chunk;
        out.chunk  = Some(rstd::move(chunk));
        return out;
    }

    static auto make_finished() -> BodyEvent {
        auto out = BodyEvent {};
        out.kind = Kind::Finished;
        return out;
    }

    static auto make_error(Error error) -> BodyEvent {
        auto out   = BodyEvent {};
        out.kind   = Kind::Error;
        out.error  = Some(rstd::move(error));
        return out;
    }
};

struct OperationState {
    using ReadyWaiter = rstd_coro::PollStateArc<rstd::Option<Error>>;
    using BodyWaiter  = rstd_coro::PollStateArc<rstd::Option<BodyEvent>>;

    Request                       request;
    Operation                     operation;
    Weak<QtNetworkDriver>         driver;
    rstd::Option<req_opt::Proxy>  proxy;
    std::atomic<bool>             finished { false };
    std::atomic<bool>             cancel_requested { false };

    OperationState(Request request,
                   Operation operation,
                   Weak<QtNetworkDriver> driver,
                   rstd::Option<req_opt::Proxy> proxy)
        : request(rstd::move(request)),
          operation(operation),
          driver(rstd::move(driver)),
          proxy(rstd::move(proxy)) {}

    void cancel();

    void start_ready_wait(ReadyWaiter waiter) {
        auto result = rstd::Option<Error> {};
        {
            auto lock = std::lock_guard { mutex };
            if (waiter->is_canceled()) return;
            if (! ready) {
                ready_waiter = Some(rstd::move(waiter));
                return;
            }
            result = rstd::move(ready_error);
        }
        waiter->set_ready(rstd::move(result));
    }

    void cancel_ready_wait(const ReadyWaiter& waiter) {
        auto lock = std::lock_guard { mutex };
        if (ready_waiter.is_some() && *ready_waiter == waiter) {
            ready_waiter = None();
        }
    }

    void complete_ready(rstd::Option<Error> error = None<Error>()) {
        auto waiter = rstd::Option<ReadyWaiter> {};
        {
            auto lock = std::lock_guard { mutex };
            if (ready) return;
            ready = true;
            if (ready_waiter.is_some()) {
                waiter = ready_waiter.take();
            } else {
                ready_error = rstd::move(error);
                return;
            }
        }

        (*waiter)->set_ready(rstd::move(error));
    }

    void start_body_wait(BodyWaiter waiter) {
        auto result = rstd::Option<BodyEvent> {};
        {
            auto lock = std::lock_guard { mutex };
            if (waiter->is_canceled()) return;
            if (! body_events.empty()) {
                auto event = rstd::move(body_events.front());
                body_events.pop_front();
                result = Some(rstd::move(event));
            } else if (body_closed) {
                result = None<BodyEvent>();
            } else if (body_waiter.is_some()) {
                result = Some(BodyEvent::make_error(
                    Error::InvalidState("Qt response body read already pending")));
            } else {
                body_waiter = Some(rstd::move(waiter));
                return;
            }
        }
        waiter->set_ready(rstd::move(result));
    }

    void cancel_body_wait(const BodyWaiter& waiter) {
        auto lock = std::lock_guard { mutex };
        if (body_waiter.is_some() && *body_waiter == waiter) {
            body_waiter = None();
        }
    }

    void push_body_event(BodyEvent event) {
        auto waiter = rstd::Option<BodyWaiter> {};
        auto result = rstd::Option<BodyEvent> {};
        auto close  = event.kind == BodyEvent::Kind::Finished ||
                     event.kind == BodyEvent::Kind::Error;
        {
            auto lock = std::lock_guard { mutex };
            if (body_closed) return;
            body_closed = close;
            if (body_waiter.is_some()) {
                waiter = body_waiter.take();
                result = Some(rstd::move(event));
            } else {
                body_events.push_back(rstd::move(event));
                return;
            }
        }

        (*waiter)->set_ready(rstd::move(result));
    }

    void close_body() {
        auto waiter = rstd::Option<BodyWaiter> {};
        {
            auto lock = std::lock_guard { mutex };
            if (body_closed) return;
            body_closed = true;
            waiter      = body_waiter.take();
        }

        if (waiter.is_some()) {
            (*waiter)->set_ready(None<BodyEvent>());
        }
    }

private:
    std::mutex                  mutex;
    bool                        ready { false };
    rstd::Option<Error>         ready_error;
    rstd::Option<ReadyWaiter>   ready_waiter;
    std::deque<BodyEvent>       body_events;
    bool                        body_closed { false };
    rstd::Option<BodyWaiter>    body_waiter;
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

class OperationReadyFuture {
public:
    using Output = rstd::Option<Error>;

    explicit OperationReadyFuture(Arc<OperationState> state)
        : m_operation(rstd::move(state)),
          m_waiter(rstd_coro::make_poll_state<rstd::Option<Error>>()) {}

    OperationReadyFuture(const OperationReadyFuture&)            = delete;
    OperationReadyFuture& operator=(const OperationReadyFuture&) = delete;

    OperationReadyFuture(OperationReadyFuture&& other) noexcept
        : m_operation(rstd::move(other.m_operation)),
          m_waiter(rstd::move(other.m_waiter)),
          m_started(other.m_started) {}

    auto operator=(OperationReadyFuture&& other) noexcept -> OperationReadyFuture& {
        if (this != &other) {
            cancel();
            m_operation = rstd::move(other.m_operation);
            m_waiter    = rstd::move(other.m_waiter);
            m_started   = other.m_started;
        }
        return *this;
    }

    ~OperationReadyFuture() { cancel(); }

    auto poll(rstd::mut_ref<OperationReadyFuture> self, rstd::task::Context& cx)
        -> rstd::task::Poll<Output> {
        auto& future = *self;
        if (! future.m_started) {
            future.m_started = true;
            future.m_operation->start_ready_wait(future.m_waiter);
        }
        return future.m_waiter->poll(cx);
    }

private:
    void cancel() {
        if (! m_waiter) return;
        m_waiter->cancel();
        if (m_operation) {
            m_operation->cancel_ready_wait(m_waiter);
        }
        m_waiter.reset();
        m_operation.reset();
    }

    Arc<OperationState>                       m_operation;
    OperationState::ReadyWaiter               m_waiter;
    bool                                      m_started { false };
};

class BodyEventFuture {
public:
    using Output = rstd::Option<BodyEvent>;

    explicit BodyEventFuture(Arc<OperationState> state)
        : m_operation(rstd::move(state)),
          m_waiter(rstd_coro::make_poll_state<rstd::Option<BodyEvent>>()) {}

    BodyEventFuture(const BodyEventFuture&)            = delete;
    BodyEventFuture& operator=(const BodyEventFuture&) = delete;

    BodyEventFuture(BodyEventFuture&& other) noexcept
        : m_operation(rstd::move(other.m_operation)),
          m_waiter(rstd::move(other.m_waiter)),
          m_started(other.m_started) {}

    auto operator=(BodyEventFuture&& other) noexcept -> BodyEventFuture& {
        if (this != &other) {
            cancel();
            m_operation = rstd::move(other.m_operation);
            m_waiter    = rstd::move(other.m_waiter);
            m_started   = other.m_started;
        }
        return *this;
    }

    ~BodyEventFuture() { cancel(); }

    auto poll(rstd::mut_ref<BodyEventFuture> self, rstd::task::Context& cx)
        -> rstd::task::Poll<Output> {
        auto& future = *self;
        if (! future.m_started) {
            future.m_started = true;
            future.m_operation->start_body_wait(future.m_waiter);
        }
        return future.m_waiter->poll(cx);
    }

private:
    void cancel() {
        if (! m_waiter) return;
        m_waiter->cancel();
        if (m_operation) {
            m_operation->cancel_body_wait(m_waiter);
        }
        m_waiter.reset();
        m_operation.reset();
    }

    Arc<OperationState>                       m_operation;
    OperationState::BodyWaiter                m_waiter;
    bool                                      m_started { false };
};

struct ReplyWakeState : public std::enable_shared_from_this<ReplyWakeState> {
    bool                            connected { false };
    bool                            ready { false };
    QPointer<QNetworkReply>         reply;
    rstd::Option<rstd::task::Waker> waker;
    QList<QMetaObject::Connection>  connections;

    explicit ReplyWakeState(QPointer<QNetworkReply> in_reply): reply(rstd::move(in_reply)) {}

    ~ReplyWakeState() {
        for (auto const& connection : connections) {
            QObject::disconnect(connection);
        }
    }

    auto is_ready() const -> bool {
        if (! reply) return true;
        return ready || reply->isFinished() || reply->bytesAvailable() > 0;
    }

    void wake() {
        ready      = true;
        auto local = waker.take();
        if (local.is_some()) {
            rstd::move(*local).wake();
        }
    }
};

class ReplyWakeFuture {
public:
    using Output = rstd::empty;

    explicit ReplyWakeFuture(QPointer<QNetworkReply> reply)
        : m_state(std::make_shared<ReplyWakeState>(rstd::move(reply))) {}

    auto poll(rstd::mut_ref<ReplyWakeFuture> self, rstd::task::Context& cx)
        -> rstd::task::Poll<Output> {
        auto& future = *self;
        if (future.m_state->is_ready()) {
            return rstd::task::Poll<Output>::Ready(rstd::empty {});
        }

        future.m_state->waker = rstd::Some(cx.waker().clone());
        if (! future.m_state->connected) {
            future.connect();
        }
        return rstd::task::Poll<Output>::Pending();
    }

private:
    std::shared_ptr<ReplyWakeState> m_state;

    void connect() {
        if (! m_state->reply) {
            m_state->wake();
            return;
        }

        m_state->connected = true;
        auto* reply_ptr    = m_state->reply.data();
        auto  self         = m_state->weak_from_this();
        auto  wake_self    = [self] {
            if (auto locked = self.lock()) {
                locked->wake();
            }
        };

        m_state->connections.append(
            QObject::connect(reply_ptr, &QNetworkReply::finished, reply_ptr, wake_self));
        m_state->connections.append(
            QObject::connect(reply_ptr, &QNetworkReply::readyRead, reply_ptr, wake_self));
        m_state->connections.append(
            QObject::connect(reply_ptr, &QIODevice::readChannelFinished, reply_ptr, wake_self));
        m_state->connections.append(
            QObject::connect(reply_ptr, &QNetworkReply::metaDataChanged, reply_ptr, wake_self));
        m_state->connections.append(
            QObject::connect(reply_ptr, &QNetworkReply::errorOccurred, reply_ptr, wake_self));
        m_state->connections.append(
            QObject::connect(reply_ptr, &QObject::destroyed, reply_ptr, wake_self));
    }
};

class QtNetworkWorker : public QObject {
    struct ReplyEntry {
        QPointer<QNetworkReply> reply;
        Arc<OperationState>     state;
    };

    QNetworkAccessManager*                         m_manager { nullptr };
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
        if (state->finished.load()) return;
        state->push_body_event(BodyEvent::make_header(read_header(reply)));
    }

    void emit_chunks(const Arc<OperationState>& state, QNetworkReply* reply) {
        if (state->finished.load() || reply == nullptr) return;

        while (reply->bytesAvailable() > 0) {
            auto chunk = reply->read(reply->bytesAvailable());
            if (chunk.isEmpty()) break;

            auto bytes = rstd::bytes::Bytes::copy_from_slice(
                rstd::slice<rstd::u8>::from_raw_parts(
                    reinterpret_cast<const rstd::u8*>(chunk.constData()),
                    static_cast<rstd::usize>(chunk.size())));
            state->push_body_event(BodyEvent::make_chunk(rstd::move(bytes)));
        }
    }

    void finish_reply(const Arc<OperationState>& state, QNetworkReply* reply) {
        if (state->finished.load()) return;

        emit_header(state, reply);
        emit_chunks(state, reply);
        state->finished.store(true);

        auto error = transport_error(reply);
        if (error.is_some()) {
            state->push_body_event(BodyEvent::make_error(rstd::move(error).unwrap()));
        } else {
            state->push_body_event(BodyEvent::make_finished());
        }
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
    }
}

export class SessionBackend : public NoCopy {
public:
    SessionBackend(): m_driver(std::make_shared<QtNetworkDriver>()) {}

    explicit SessionBackend(QObject* parent)
        : m_driver(parent == nullptr ? std::make_shared<QtNetworkDriver>() : nullptr),
          m_manager(parent == nullptr ? nullptr : new QNetworkAccessManager(parent)) {}

    explicit SessionBackend(QNetworkAccessManager* manager): m_manager(manager) {}

    ~SessionBackend() = default;

    template<typename... Args>
    static auto make(Args&&... args) -> Arc<SessionBackend> {
        return make_arc<SessionBackend>(rstd::forward<Args>(args)...);
    }

    auto start_request(const Request& req, Operation operation, rstd::Option<rstd::bytes::Bytes> body)
        -> coro<Result<ResponseBackend>>;

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
    std::shared_ptr<QtNetworkDriver>    m_driver;
    std::unique_ptr<QNetworkAccessManager> m_owned_manager;
    QPointer<QNetworkAccessManager>        m_manager;
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
          m_reply(other.m_reply),
          m_header(rstd::move(other.m_header)) {
        other.m_reply = nullptr;
    }

    auto operator=(ResponseBackend&& other) noexcept -> ResponseBackend& {
        if (this == &other) return *this;

        cancel();
        m_req         = rstd::move(other.m_req);
        m_operation   = other.m_operation;
        m_state       = rstd::move(other.m_state);
        m_reply       = other.m_reply;
        m_header      = rstd::move(other.m_header);
        other.m_reply = nullptr;
        return *this;
    }

    auto header() const -> const HttpHeader& { return m_header; }
    auto code() const -> rstd::Option<i32> {
        if (m_header.start.is_some()) {
            if (auto* start = std::get_if<HttpHeader::Status>(&*m_header.start)) {
                return Some<i32>(start->code);
            }
        }

        auto* reply = m_reply.data();
        if (! reply) return None<i32>();

        auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (! status.isValid()) return None<i32>();
        return Some<i32>(status.toInt());
    }

    auto bytes() -> coro<Result<rstd::bytes::Bytes>>;
    auto text() -> coro<Result<std::string>>;

    auto is_finished() const -> bool {
        if (m_state) {
            return m_state->finished.load();
        }

        auto* reply = m_reply.data();
        return reply == nullptr || reply->isFinished();
    }
    auto request() const -> const Request& { return m_req; }
    auto operation() const -> Operation { return m_operation; }

    void cancel() {
        if (m_state) {
            m_state->cancel();
            return;
        }

        auto* reply = m_reply.data();
        if (reply != nullptr && ! reply->isFinished()) {
            reply->abort();
        }
    }

private:
    static auto make_response(const Request&, Operation, QNetworkReply*) -> Arc<ResponseBackend>;

    explicit ResponseBackend(Arc<OperationState> state)
        : m_req(state->request.clone()),
          m_operation(state->operation),
          m_state(rstd::move(state)) {}

    ResponseBackend(const Request& req, Operation operation, QNetworkReply* reply)
        : m_req(req.clone()), m_operation(operation), m_reply(reply) {}

    void update_header();
    auto transport_error() const -> rstd::Option<Error>;

private:
    Request                 m_req;
    Operation               m_operation;
    Arc<OperationState>     m_state;
    QPointer<QNetworkReply> m_reply;
    HttpHeader              m_header;
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
    auto prepared = prepare_req(req);
    auto request  = to_qnetwork_request(prepared);
    if (request.is_err()) {
        co_return Result<ResponseBackend>(Err(rstd::move(request).unwrap_err()));
    }

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

    co_return Result<ResponseBackend>(Ok(ResponseBackend(prepared, operation, reply)));
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

    auto state = std::make_shared<OperationState>(
        rstd::move(prepared),
        operation,
        Weak<QtNetworkDriver>(m_driver),
        rstd::move(proxy));

    if (! m_driver->start(state, rstd::move(body))) {
        state->close_body();
        co_return Result<ResponseBackend>(
            Err(Error::InvalidState("Qt network driver is not running")));
    }

    auto ready_error = co_await OperationReadyFuture { state };
    if (ready_error.is_some()) {
        co_return Result<ResponseBackend>(
            Err(rstd::move(ready_error).unwrap_unchecked()));
    }

    co_return Result<ResponseBackend>(Ok(ResponseBackend(rstd::move(state))));
}

auto SessionBackend::get(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    auto res = co_await start_request(req, Operation::GetOperation, None<rstd::bytes::Bytes>());
    if (res.is_err()) {
        co_return Result<Arc<ResponseBackend>>(Err(rstd::move(res).unwrap_err()));
    }
    co_return Result<Arc<ResponseBackend>>(
        Ok(make_arc<ResponseBackend>(rstd::move(res).unwrap())));
}

auto SessionBackend::post(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    co_return co_await post(req, rstd::bytes::Bytes::make());
}

auto SessionBackend::post(const Request& req, rstd::bytes::Bytes body) -> coro<Result<Arc<ResponseBackend>>> {
    auto res = co_await start_request(req, Operation::PostOperation, Some(rstd::move(body)));
    if (res.is_err()) {
        co_return Result<Arc<ResponseBackend>>(Err(rstd::move(res).unwrap_err()));
    }
    co_return Result<Arc<ResponseBackend>>(
        Ok(make_arc<ResponseBackend>(rstd::move(res).unwrap())));
}

void SessionBackend::set_proxy(const req_opt::Proxy& proxy) {
    m_proxy = Some(proxy.clone());

    auto* manager = this->manager();
    if (manager == nullptr) return;
    apply_proxy(manager, proxy);
}

void SessionBackend::set_verify_certificate(bool value) { m_verify_certificate = value; }

auto ResponseBackend::make_response(const Request& req, Operation operation, QNetworkReply* reply)
    -> Arc<ResponseBackend> {
    return Arc<ResponseBackend>(new ResponseBackend(req, operation, reply));
}

void ResponseBackend::update_header() {
    auto* reply = m_reply.data();
    if (reply == nullptr) return;

    m_header = read_header(reply);
}

auto ResponseBackend::transport_error() const -> rstd::Option<Error> {
    return qt_network::transport_error(m_reply.data());
}

auto ResponseBackend::bytes() -> coro<Result<rstd::bytes::Bytes>> {
    rstd::bytes::BytesMut out = rstd::bytes::BytesMut::with_capacity(ReadSize);

	    if (m_state) {
	        for (;;) {
	            auto item = co_await BodyEventFuture { m_state };
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
                co_return Result<rstd::bytes::Bytes>(
                    Err(rstd::move(event.error).unwrap_unchecked()));
            }
            }
        }
    }

    for (;;) {
        auto* reply = m_reply.data();
        if (reply == nullptr) {
            co_return Result<rstd::bytes::Bytes>(
                Err(Error::InvalidState("QNetworkReply was destroyed")));
        }

        update_header();

        while (reply->bytesAvailable() > 0) {
            auto chunk = reply->read(qMin<qint64>(reply->bytesAvailable(), ReadSize));
            out.extend_from_slice(reinterpret_cast<const u8*>(chunk.constData()),
                                  static_cast<usize>(chunk.size()));
        }

        if (reply->isFinished()) {
            update_header();
            auto error = transport_error();
            if (error.is_some()) {
                co_return Result<rstd::bytes::Bytes>(Err(rstd::move(error).unwrap()));
            }
            co_return Result<rstd::bytes::Bytes>(Ok(out.freeze()));
        }

        co_await ReplyWakeFuture { m_reply };
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
