module;
#include <memory>
#include <string>
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
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

    auto poll(rstd::pin::Pin<rstd::mut_ref<ReplyWakeFuture>> self, rstd::task::Context& cx)
        -> rstd::task::Poll<Output> {
        auto& future = *self.get_unchecked_mut();
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

export class SessionBackend : public NoCopy {
public:
    SessionBackend(): SessionBackend(static_cast<QObject*>(nullptr)) {}

    explicit SessionBackend(QObject* parent)
        : m_owned_manager(parent == nullptr ? std::make_unique<QNetworkAccessManager>() : nullptr),
          m_manager(parent == nullptr ? m_owned_manager.get() : new QNetworkAccessManager(parent)) {
    }

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

private:
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
          m_reply(other.m_reply),
          m_header(rstd::move(other.m_header)) {
        other.m_reply = nullptr;
    }

    auto operator=(ResponseBackend&& other) noexcept -> ResponseBackend& {
        if (this == &other) return *this;

        cancel();
        m_req       = rstd::move(other.m_req);
        m_operation = other.m_operation;
        m_reply     = other.m_reply;
        m_header    = rstd::move(other.m_header);
        other.m_reply = nullptr;
        return *this;
    }

    auto header() const -> const HttpHeader& { return m_header; }
    auto code() const -> rstd::Option<i32> {
        auto* reply = m_reply.data();
        if (! reply) return None<i32>();

        auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (! status.isValid()) {
            if (m_header.start.is_some()) {
                if (auto* start = std::get_if<HttpHeader::Status>(&*m_header.start)) {
                    return Some<i32>(start->code);
                }
            }
            return None<i32>();
        }
        return Some<i32>(status.toInt());
    }

    auto bytes() -> coro<Result<rstd::bytes::Bytes>>;
    auto text() -> coro<Result<std::string>>;

    auto is_finished() const -> bool {
        auto* reply = m_reply.data();
        return reply == nullptr || reply->isFinished();
    }
    auto request() const -> const Request& { return m_req; }
    auto operation() const -> Operation { return m_operation; }

    void cancel() {
        auto* reply = m_reply.data();
        if (reply != nullptr && ! reply->isFinished()) {
            reply->abort();
        }
    }

private:
    static auto make_response(const Request&, Operation, QNetworkReply*) -> Arc<ResponseBackend>;

    ResponseBackend(const Request& req, Operation operation, QNetworkReply* reply)
        : m_req(req.clone()), m_operation(operation), m_reply(reply) {}

    void update_header();
    auto transport_error() const -> rstd::Option<Error>;

private:
    Request                 m_req;
    Operation               m_operation;
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

auto SessionBackend::start_request(const Request& req, Operation operation,
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
        QByteArray payload;
        if (body.is_some()) {
            auto bytes = rstd::move(body).unwrap();
            payload    = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<qsizetype>(bytes.size()));
        }
        reply = manager->post(rstd::move(request).unwrap(), payload);
    } else {
        reply = manager->get(rstd::move(request).unwrap());
    }

    if (reply == nullptr) {
        co_return Result<ResponseBackend>(Err(Error::InvalidState("Qt did not create a reply")));
    }

    co_return Result<ResponseBackend>(Ok(ResponseBackend(prepared, operation, reply)));
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

void SessionBackend::set_verify_certificate(bool value) { m_verify_certificate = value; }

auto ResponseBackend::make_response(const Request& req, Operation operation, QNetworkReply* reply)
    -> Arc<ResponseBackend> {
    return Arc<ResponseBackend>(new ResponseBackend(req, operation, reply));
}

void ResponseBackend::update_header() {
    auto* reply = m_reply.data();
    if (reply == nullptr) return;

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) {
        m_header.start = Some<HttpHeader::Start>(
            HttpHeader::Status { .version = "HTTP", .code = status.toInt() });
    }

    m_header.fields.clear();
    for (auto const& pair : reply->rawHeaderPairs()) {
        m_header.fields.push_back(HttpHeader::Field {
            .name  = std::string(pair.first.constData(), static_cast<usize>(pair.first.size())),
            .value = std::string(pair.second.constData(), static_cast<usize>(pair.second.size())),
        });
    }
}

auto ResponseBackend::transport_error() const -> rstd::Option<Error> {
    auto* reply = m_reply.data();
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

auto ResponseBackend::bytes() -> coro<Result<rstd::bytes::Bytes>> {
    rstd::bytes::BytesMut out = rstd::bytes::BytesMut::with_capacity(ReadSize);

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
