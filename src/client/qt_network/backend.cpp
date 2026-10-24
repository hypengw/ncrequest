module;
#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QThread>
#include <QHttpHeaders>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslError>
#include <string.h>
module ncrequest;
import :client.qt_network.backend;
import :client.io;
import :client.qt_network.websocket;
import :client.qt_network.dispatch;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client::qt_network
{
namespace lf = lifetchto;
namespace h  = lifetchto::http;
auto proxy_capabilities() -> h::ProxyCapabilities { return { .http = true, .socks5h = true }; }
auto proxy_configuration(const h::ProxyOptions& options) -> QNetworkProxy {
    auto endpoint = options.explicit_endpoint();
    if (endpoint.is_none()) return QNetworkProxy(QNetworkProxy::NoProxy);
    auto host = endpoint->host_name();
    return QNetworkProxy(endpoint->type() == h::ProxyType::Http ? QNetworkProxy::HttpProxy
                                                                : QNetworkProxy::Socks5Proxy,
                         QString::fromUtf8(reinterpret_cast<const char*>(host.as_str().data()),
                                           qsizetype(host.len().to_primitive())),
                         endpoint->port().to_primitive());
}
auto qt_bytes(const Bytes& bytes) -> QByteArray {
    return QByteArray(reinterpret_cast<const char*>(bytes.as_slice().as_raw_ptr()),
                      qsizetype(bytes.len().to_primitive()));
}
auto tls_configuration(const h::TlsOptions& options) -> lf::Result<QSslConfiguration> {
    auto                   config = QSslConfiguration::defaultConfiguration();
    QList<QSslCertificate> roots;
    if (options.use_system_roots) roots = config.caCertificates();
    for (const auto& pem : options.ca_pem) {
        auto parsed = QSslCertificate::fromData(qt_bytes(pem), QSsl::Pem);
        if (parsed.isEmpty())
            return Err(lf::Error::InvalidOptions("Qt cannot parse CA certificates"_Str));
        roots.append(parsed);
    }
    config.setCaCertificates(roots);
    config.setPeerVerifyMode(options.verification == h::TlsVerification::Disabled
                                 ? QSslSocket::VerifyNone
                                 : QSslSocket::VerifyPeer);
    if (options.identity.is_some()) {
        auto chain =
            QSslCertificate::fromData(qt_bytes(options.identity->certificate_chain), QSsl::Pem);
        auto    encoded = qt_bytes(options.identity->private_key);
        QSslKey key(encoded, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (key.isNull()) key = QSslKey(encoded, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        if (chain.isEmpty() || key.isNull())
            return Err(lf::Error::InvalidOptions(
                "Qt requires a PEM certificate chain and RSA/EC private key"_Str));
        config.setLocalCertificateChain(chain);
        config.setPrivateKey(key);
    }
    return Ok(rstd::move(config));
}
auto text(const QByteArray& value) -> String {
    return String::make(
        rstd::str_::from_utf8(
            slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(value.constData()),
                                      usize(value.size())))
            .unwrap());
}
auto failure(QNetworkReply* reply) -> lf::Error {
    return lf::Error::Extension("qt"_Str,
                                Arc<rstd::dyn<rstd::error::Error>>::make(Error(
                                    i32(reply->error()), text(reply->errorString().toUtf8()))));
}
struct Fields {
    Vec<rstd::tuple<Arc<Exchange>, Dispatch>> queued;
    bool                                      closed {}, finished {};
    usize                                     handles { 1 };
};
struct BackendState {
    Options                   options;
    QSslConfiguration         ssl;
    QNetworkProxy             proxy;
    h::BackendIdentity        identity;
    Dispatch                  dispatch;
    WebSocketService          websockets;
    rstd::sync::Mutex<Fields> fields { Fields {} };
    Option<lf::TaskControl>   worker;
    lf::Event                 changed;
    BackendState(Options options, QSslConfiguration ssl, QNetworkProxy proxy)
        : options(rstd::move(options)),
          ssl(ssl),
          proxy(proxy),
          websockets(rstd::move(ssl), rstd::move(proxy), this->options.tls.verification,
                     dispatch.clone()) {}
};
void stop(const Arc<BackendState>& state) {
    state->websockets.stop();
    state->fields.lock().unwrap()->closed = true;
    state->changed.notify();
    state->dispatch.notify();
}
void fail_pending(const Arc<BackendState>& state, const lf::Error& error) {
    state->websockets.dispose();
    Vec<rstd::tuple<Arc<Exchange>, Dispatch>> queued;
    {
        auto f    = state->fields.lock().unwrap();
        f->closed = true;
        queued    = rstd::move(f->queued);
    }
    for (const auto& queued_exchange : queued) {
        auto& exchange = queued_exchange.get<0>();
        exchange->fail(error.clone());
        exchange->complete();
    }
    state->changed.notify();
    state->dispatch.notify();
}
class UploadDevice : public QIODevice {
    Arc<Exchange> exchange_;
    bool          eof_ {};

public:
    explicit UploadDevice(Arc<Exchange> exchange): exchange_(rstd::move(exchange)) {
        open(ReadOnly);
    }
    bool   isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        return qint64((exchange_->window - exchange_->outgoing->available()).to_primitive()) +
               QIODevice::bytesAvailable();
    }
    bool   atEnd() const override { return eof_ && bytesAvailable() == 0; }
    qint64 size() const override {
        auto n = exchange_->prepared.upload().length;
        return n.is_some() ? qint64(n->to_primitive()) : qint64(0);
    }
    qint64 readData(char* data, qint64 maximum) override {
        if (maximum <= 0) return 0;
        auto ready = exchange_->outgoing->try_next(usize(maximum));
        if (ready.is_err()) {
            exchange_->fail(rstd::move(ready).unwrap_err());
            return -1;
        }
        if (ready->is_none()) {
            exchange_->demand();
            return 0;
        }
        auto part = ready->take().unwrap();
        if (part.is_none()) {
            eof_ = true;
            return -1;
        }
        ::memcpy(data, part->as_slice().as_raw_ptr(), part->len().to_primitive());
        return qint64(part->len().to_primitive());
    }
    qint64 writeData(const char*, qint64) override { return -1; }
    void   wake() { emit readyRead(); }
};
struct Active {
    Arc<Exchange>           exchange;
    QPointer<QNetworkReply> reply;
    QPointer<UploadDevice>  upload;
    bool                    head {}, deleting {};
    Dispatch                dispatch;
    u64                     received {};
    Option<u64>             length;
    explicit Active(Arc<Exchange> exchange, Dispatch dispatch)
        : exchange(rstd::move(exchange)), dispatch(rstd::move(dispatch)) {}
};
class Router : public QObject {
    Arc<BackendState>      state_;
    QNetworkAccessManager* manager_;
    QEventLoop*            loop_;
    Vec<Box<Active>>       active_;
    void                   head(Active& active) {
        if (active.head || ! active.reply) return;
        auto status = active.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (! status.isValid() || status.toInt() < 200) return;
        lihttpto::Headers headers;
        usize             metadata {};
        for (const auto& pair : active.reply->headers().toListOfPairs()) {
            metadata += usize(pair.first.size() + pair.second.size() + 4);
            if (metadata > state_->options.head_limit) {
                active.exchange->fail(lf::Error::TooLarge());
                return;
            }
            auto name  = rstd::str_::from_utf8(slice<u8>::from_raw_parts(
                reinterpret_cast<const rstd::byte*>(pair.first.constData()),
                usize(pair.first.size())));
            auto field = name.is_ok()
                             ? lihttpto::Header::make(
                                   *name,
                                   slice<u8>::from_raw_parts(
                                       reinterpret_cast<const rstd::byte*>(pair.second.constData()),
                                       usize(pair.second.size())))
                             : rstd::Result<lihttpto::Header, lihttpto::HeaderError>(
                                   Err(lihttpto::HeaderError::InvalidName()));
            if (field.is_err()) {
                active.exchange->fail(h::protocol_error("invalid Qt response field"_Str));
                return;
            }
            headers.push(rstd::move(field).unwrap());
        }
        auto parsed = lihttpto::StatusCode::make(u16(status.toInt()));
        if (parsed.is_err()) {
            active.exchange->fail(h::protocol_error("invalid Qt status"_Str));
            return;
        }
        if (active.exchange->prepared.parts().method.as_ref() != "HEAD"_str &&
            status.toInt() != 204 && status.toInt() != 304) {
            auto length = lihttpto::content_length(headers);
            if (length.is_err()) {
                active.exchange->fail(
                    h::protocol_error("invalid Qt content length"_Str, Some(length.unwrap_err())));
                return;
            }
            active.length = *length;
        }
        active.head = true;
        {
            auto f                = active.exchange->fields.lock().unwrap();
            f->trailers_available = ! headers.contains("transfer-encoding"_str);
            f->head               = Some(lihttpto::ResponseHead {
                rstd::move(parsed).unwrap(), rstd::move(headers), {}, {} });
        }
        active.exchange->notify();
    }
    void dispose(Active& active) {
        if (active.deleting) return;
        active.deleting = true;
        auto* reply     = active.reply.data();
        if (! reply) {
            active.exchange->complete();
            return;
        }
        QObject::disconnect(reply, nullptr, this, nullptr);
        auto exchange = make_clone_tuple(active.exchange.clone());
        QObject::connect(
            reply,
            &QObject::destroyed,
            this,
            [exchange] {
                exchange.get<0>()->complete();
            },
            Qt::QueuedConnection);
        reply->abort();
        reply->deleteLater();
    }
    void pump(Active& active) {
        if (active.deleting) return;
        auto* reply = active.reply.data();
        if (! reply) {
            active.exchange->fail(lf::Error::Closed());
            active.exchange->complete();
            return;
        }
        bool stop;
        {
            auto f = active.exchange->fields.lock().unwrap();
            stop   = f->stopped || f->error.is_some();
        }
        if (stop) {
            dispose(active);
            return;
        }
        head(active);
        unsigned batches {};
        while (batches++ < 4 && active.head && reply->bytesAvailable() > 0 &&
               active.exchange->incoming->available() > usize()) {
            auto maximum = rstd::min(active.exchange->incoming->available(), usize(16384));
            auto part    = reply->read(qint64(maximum.to_primitive()));
            if (part.isEmpty()) break;
            active.received += u64(part.size());
            auto sent = active.exchange->incoming->try_send(
                Bytes::copy_from_slice(slice<u8>::from_raw_parts(
                    reinterpret_cast<const rstd::byte*>(part.constData()), usize(part.size()))));
            if (sent.is_err()) {
                active.exchange->fail(rstd::move(sent).unwrap_err());
                dispose(active);
                return;
            }
        }
        if (active.head && reply->bytesAvailable() > 0 &&
            active.exchange->incoming->available() > usize())
            active.dispatch.notify();
        if (active.upload) {
            auto readable = active.exchange->outgoing->readable();
            if (readable.is_err() || *readable) active.upload->wake();
        }
        if (! reply->isFinished() || reply->bytesAvailable() > 0) return;
        auto code         = reply->error();
        bool status_error = active.head && ((code >= QNetworkReply::ContentAccessDenied &&
                                             code <= QNetworkReply::UnknownContentError) ||
                                            (code >= QNetworkReply::InternalServerError &&
                                             code <= QNetworkReply::UnknownServerError));
        if (code != QNetworkReply::NoError && ! status_error) active.exchange->fail(failure(reply));
        if (active.length.is_some() && active.received != *active.length)
            active.exchange->fail(h::protocol_error("truncated Qt response"_Str,
                                                    Some(lihttpto::DecodeError::Truncated)));
        if (! active.head)
            active.exchange->fail(h::protocol_error("Qt completed without response head"_Str));
        dispose(active);
    }
    void start(Arc<Exchange> exchange, Dispatch dispatch) {
        auto            active = Box<Active>::make(exchange.clone(), rstd::move(dispatch));
        auto&           parts  = exchange->prepared.parts();
        QNetworkRequest request(
            QUrl::fromEncoded(QByteArray(reinterpret_cast<const char*>(parts.url.as_ref().data()),
                                         qsizetype(parts.url.as_ref().size().to_primitive()))));
        request.setSslConfiguration(state_->ssl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                             QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QHttpHeaders headers;
        for (const auto& field : parts.headers)
            headers.append(
                QByteArray(reinterpret_cast<const char*>(field.name.as_str().data()),
                           qsizetype(field.name.as_str().size().to_primitive())),
                QByteArray(reinterpret_cast<const char*>(field.value.as_slice().as_raw_ptr()),
                           qsizetype(field.value.as_slice().len().to_primitive())));
        request.setHeaders(headers);
        if (! parts.headers.contains("accept-encoding"_str))
            request.setRawHeader("Accept-Encoding", "identity");
        if (exchange->prepared.upload().length.is_some())
            request.setHeader(QNetworkRequest::ContentLengthHeader,
                              qlonglong(exchange->prepared.upload().length->to_primitive()));
        auto method = parts.method.as_ref();
        if (method == "HEAD"_str)
            active->reply = manager_->head(request);
        else if (exchange->prepared.upload().length == Some(u64()))
            active->reply =
                manager_->sendCustomRequest(request,
                                            QByteArray(reinterpret_cast<const char*>(method.data()),
                                                       qsizetype(method.size().to_primitive())),
                                            QByteArray {});
        else {
            active->upload = new UploadDevice(exchange.clone());
            active->reply =
                manager_->sendCustomRequest(request,
                                            QByteArray(reinterpret_cast<const char*>(method.data()),
                                                       qsizetype(method.size().to_primitive())),
                                            active->upload.data());
            active->upload->setParent(active->reply);
        }
        active->reply->setReadBufferSize(qint64(exchange->window.to_primitive()));
        if (state_->options.tls.verification == h::TlsVerification::ChainOnly) {
            auto* reply = active->reply.data();
            QObject::connect(
                reply, &QNetworkReply::sslErrors, this, [reply](const QList<QSslError>& errors) {
                    QList<QSslError> ignored;
                    for (const auto& error : errors)
                        if (error.error() == QSslError::HostNameMismatch) ignored.append(error);
                    if (! ignored.isEmpty()) reply->ignoreSslErrors(ignored);
                });
        }
        auto* reply = active->reply.data();
        auto  wake  = [exchange = make_clone_tuple(exchange.clone())] {
            exchange.get<0>()->notify();
        };
        QObject::connect(reply, &QNetworkReply::metaDataChanged, this, wake);
        QObject::connect(reply, &QNetworkReply::readyRead, this, wake);
        QObject::connect(reply, &QNetworkReply::finished, this, wake);
        QObject::connect(
            reply, &QNetworkReply::errorOccurred, this, [wake](QNetworkReply::NetworkError) {
                wake();
            });
        active->dispatch.notify();
        active_.push(rstd::move(active));
    }

public:
    Router(Arc<BackendState> state, QNetworkAccessManager* manager, QEventLoop* loop)
        : QObject(manager), state_(rstd::move(state)), manager_(manager), loop_(loop) {
        auto holder = make_clone_tuple(state_.clone());
        QObject::connect(this, &QObject::destroyed, [holder] {
            auto state                              = holder.get<0>().clone();
            state->fields.lock().unwrap()->finished = true;
            state->changed.notify();
        });
        state_->dispatch.bind(this, Callback<void(Vec<usize>)>([this](Vec<usize> ids) {
                                  advance(rstd::move(ids));
                              }));
    }
    void advance(Vec<usize> ids) {
        Vec<rstd::tuple<Arc<Exchange>, Dispatch>> queued;
        bool                                      closing;
        {
            auto f  = state_->fields.lock().unwrap();
            queued  = rstd::move(f->queued);
            closing = f->closed;
        }
        if (closing) state_->websockets.stop();
        state_->websockets.advance(this, ids.as_slice());
        for (auto& entry : queued) {
            auto exchange = rstd::move(entry.get<0>());
            if (closing) {
                exchange->stop();
                exchange->complete();
            } else
                start(rstd::move(exchange), rstd::move(entry.get<1>()));
        }
        for (usize i {}; i < active_.len();) {
            auto& active = *active_[i];
            bool  ready  = closing;
            for (auto id : ids) ready = ready || id == active.dispatch.id();
            if (ready) {
                if (closing) active.exchange->stop();
                pump(active);
            }
            if (active.exchange->fields.lock().unwrap()->done)
                active_.remove(i);
            else
                ++i;
        }
        if (closing && active_.is_empty() && state_->websockets.empty()) {
            state_->dispatch.close();
            if (loop_)
                loop_->quit();
            else
                deleteLater();
        }
    }

    ~Router() {
        state_->dispatch.close();
        state_->websockets.dispose();
        Vec<rstd::tuple<Arc<Exchange>, Dispatch>> queued;
        {
            auto f    = state_->fields.lock().unwrap();
            f->closed = true;
            queued    = rstd::move(f->queued);
        }
        for (auto& entry : queued) {
            auto& exchange = entry.get<0>();
            exchange->stop();
            exchange->complete();
        }
        for (auto& active : active_) {
            if (active->reply) {
                QObject::disconnect(active->reply, nullptr, this, nullptr);
                active->reply->abort();
                delete active->reply.data();
            }
            active->exchange->stop();
            active->exchange->complete();
        }
    }
};
struct Worker {
    Arc<BackendState>          state;
    rstd::async::RuntimeHandle runtime;
    void                       request_stop(lf::StopReason) const { stop(state); }
    auto                       run(lf::Cancellation) const -> coro<lf::Result<empty>> {
        if (state->options.manager) {
            for (;;) {
                auto event = state->changed.listen();
                if (state->fields.lock().unwrap()->finished) break;
                co_await rstd::move(event);
            }
            co_return Ok(empty {});
        }
        auto job = runtime.spawn_blocking([state = state.clone()] {
            QEventLoop            loop;
            QNetworkAccessManager manager;
            manager.setProxy(state->proxy);
            {
                Router router(state.clone(), &manager, &loop);
                loop.exec();
            }
            return lf::Result<empty>(Ok(empty {}));
        });
        if (job.is_err()) {
            auto error = lf::Error::Connect(job.unwrap_err());
            fail_pending(state, error);
            co_return Err(rstd::move(error));
        }
        auto joined = co_await rstd::move(job).unwrap();
        if (joined.is_err()) {
            fail_pending(state, lf::Error::Closed());
            co_return Err(lf::Error::Closed());
        }
        co_return rstd::move(joined).unwrap();
    }
};
struct Owner {
    Arc<BackendState> state;
    void              request_stop(lf::StopReason) const { stop(state); }
    auto              release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        stop(state);
        if (state->worker.is_some()) co_return co_await state->worker->wait();
        co_return Ok(empty {});
    }
};
struct Progress {
    Arc<Exchange> exchange;
    Dispatch      dispatch;
    void          request_stop(lf::StopReason) const { exchange->stop(); }
    auto          run(lf::Cancellation cancellation) const -> coro<lf::Result<empty>> {
        for (;;) {
            auto event    = exchange->changed.listen();
            auto incoming = exchange->incoming->changed();
            auto outgoing = exchange->outgoing->changed();
            if (exchange->fields.lock().unwrap()->done || cancellation.is_canceled())
                co_return Ok(empty {});
            dispatch.notify();
            auto queues = rstd::async::select(rstd::move(incoming), rstd::move(outgoing));
            (void)co_await rstd::async::select(rstd::move(event), rstd::move(queues));
        }
    }
};
struct Driver {
    Arc<BackendState> backend;
    Arc<Exchange>     exchange;
    Dispatch          dispatch;
    void              request_stop(lf::StopReason r) const { exchange->stop(r); }
    auto              release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        auto result = co_await release_exchange(exchange.clone());
        if (! exchange->fields.lock().unwrap()->dispatched) exchange->complete();
        if (exchange->progress.is_some()) (void)co_await exchange->progress->wait();
        co_return rstd::move(result);
    }
    auto run(lf::TransferContext context, lf::OutputSender<h::ResponseMessage> output) const
        -> coro<lf::ProtocolCompletion> {
        auto begun = co_await begin_exchange(exchange.clone(), context.clone());
        if (begun.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(begun).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        auto bridge = context.resources().spawn(Progress { exchange.clone(), dispatch.clone() });
        if (bridge.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(bridge).unwrap_err().error),
                                               lf::RemoteOutcome::NotDispatched };
        exchange->progress = Some(rstd::move(bridge).unwrap());
        {
            auto f = backend->fields.lock().unwrap();
            if (f->closed)
                co_return lf::ProtocolCompletion { Err(lf::Error::Closed()),
                                                   lf::RemoteOutcome::NotDispatched };
            exchange->fields.lock().unwrap()->dispatched = true;
            f->queued.push(rstd::make_tuple(exchange.clone(), dispatch.clone()));
        }
        dispatch.notify();
        co_return co_await deliver(exchange.clone(), rstd::move(output));
    }
};
} // namespace ncrequest::client::qt_network
namespace rstd
{
template<>
struct Impl<lifetchto::ManagedResource, ncrequest::client::qt_network::Owner>
    : ImplBase<ncrequest::client::qt_network::Owner> {
    void request_stop(lifetchto::StopReason r) const { this->self().request_stop(r); }
    auto release(lifetchto::ReleaseContext c) const { return this->self().release(c); }
};
} // namespace rstd
namespace ncrequest::client::qt_network
{
Backend::Backend(Arc<BackendState> s, Arc<lf::ResourceLease<empty>> r)
    : state_(rstd::move(s)), registration_(rstd::move(r)) {}
Backend::Backend(Backend&&) noexcept = default;
void drop(const Arc<BackendState>& s) {
    if (! s) return;
    bool last;
    {
        auto f = s->fields.lock().unwrap();
        last   = --f->handles == usize();
    }
    if (last) stop(s);
}
Backend::~Backend() { drop(state_); }
auto Backend::operator=(Backend&& other) noexcept -> Backend& {
    if (this != &other) {
        drop(state_);
        state_        = rstd::move(other.state_);
        registration_ = rstd::move(other.registration_);
    }
    return *this;
}
auto Backend::clone() const -> Backend {
    {
        auto f = state_->fields.lock().unwrap();
        ++f->handles;
    }
    return Backend(state_.clone(), registration_.clone());
}
auto Backend::make(lf::ResourceContext resources, Options options) -> lf::Result<Backend> {
    if (! QCoreApplication::instance())
        return Err(lf::Error::InvalidOptions("Qt requires QCoreApplication"_Str));
    if (options.window == usize() || options.window > usize(0x7fffffff) ||
        options.head_limit < usize(16) || options.head_limit > usize(0x7fffffff))
        return Err(lf::Error::InvalidOptions("invalid Qt limits"_Str));
    if (options.manager && options.manager->thread() != QThread::currentThread())
        return Err(lf::Error::InvalidOptions("create Qt backend on the manager thread"_Str));
    auto tcp = options.tcp.validate({});
    if (tcp.is_err()) return Err(rstd::move(tcp).unwrap_err());
    if (options.low_speed.is_some())
        return Err(lf::Error::Unsupported(
            "QNetworkAccessManager does not expose a low-speed rate policy"_Str));
    if (options.connect_timeout.is_some())
        return Err(
            lf::Error::Unsupported("QNetworkAccessManager does not expose a connect deadline"_Str));
    auto proxy = options.proxy.validate(proxy_capabilities());
    if (proxy.is_err()) return Err(rstd::move(proxy).unwrap_err());
    auto native_proxy = proxy_configuration(options.proxy);
    if (options.manager &&
        (options.manager->proxyFactory() || options.manager->proxy() != native_proxy))
        return Err(lf::Error::Unsupported(
            "external Qt manager proxy must already match the immutable backend profile"_Str));
    auto tls = tls_configuration(options.tls);
    if (tls.is_err()) return Err(rstd::move(tls).unwrap_err());
    auto* manager = options.manager;
    auto  state   = Arc<BackendState>::make(
        rstd::move(options), rstd::move(tls).unwrap(), rstd::move(native_proxy));
    auto attached = resources.attach(
        empty {}, Arc<rstd::dyn<lf::ManagedResource>>::make(Owner { state.clone() }));
    if (attached.is_err()) return Err(rstd::move(attached).unwrap_err().error);
    auto registration = Arc<lf::ResourceLease<empty>>::make(rstd::move(attached).unwrap());
    auto worker       = resources.spawn(Worker { state.clone(), resources.runtime() });
    if (worker.is_err()) return Err(rstd::move(worker).unwrap_err().error);
    state->worker = Some(rstd::move(worker).unwrap());
    if (manager) new Router(state.clone(), manager, nullptr);
    return Ok(Backend(rstd::move(state), rstd::move(registration)));
}
auto Backend::backend() const -> Arc<rstd::dyn<h::ExchangeBackend>> {
    return Arc<rstd::dyn<h::ExchangeBackend>>::make(clone());
}
auto Backend::shared_binding() const -> ncrequest::BackendBinding {
    return ncrequest::BackendBinding::Shared(backend(), Some(state_->websockets.backend()));
}
auto Backend::owned_binding() const -> ncrequest::BackendBinding {
    return ncrequest::BackendBinding::Owned(backend(), Some(state_->websockets.backend()));
}
auto Backend::capabilities() const -> h::BackendCapabilities {
    return { .http1            = true,
             .https            = QSslSocket::supportsSsl(),
             .tls_custom_ca    = QSslSocket::supportsSsl(),
             .tls_identity     = QSslSocket::supportsSsl(),
             .tls_verification = QSslSocket::supportsSsl(),
             .proxy            = proxy_capabilities() };
}
auto Backend::prepare(lihttpto::RequestParts p, h::UploadMetadata u, h::RouteChoice r) const
    -> lf::Result<h::PreparedExchange> {
    if (state_->fields.lock().unwrap()->closed) return Err(lf::Error::Closed());
    auto supported = capabilities().validate(p, u, r);
    if (supported.is_err()) return Err(rstd::move(supported).unwrap_err());
    auto proxy = state_->options.proxy.select(p.url);
    if (proxy.is_err()) return Err(rstd::move(proxy).unwrap_err());
    auto prepared = prepare_exchange(state_->identity.clone(),
                                     rstd::move(p),
                                     rstd::move(u),
                                     rstd::move(r),
                                     state_->options.head_limit);
    if (prepared.is_ok()) prepared->set_proxy(rstd::move(proxy).unwrap());
    return prepared;
}
auto Backend::operation(h::ExchangeInput input) const
    -> rstd::Result<lf::PreparedOperation<h::ResponseMessage>, lf::Rejected<h::ExchangeInput>> {
    if (state_->fields.lock().unwrap()->closed)
        return Err(lf::Rejected<h::ExchangeInput> { lf::Error::Closed(), rstd::move(input) });
    if (! input.prepared.backend_identity().matches(state_->identity))
        return Err(lf::Rejected<h::ExchangeInput> {
            lf::Error::InvalidOptions("exchange belongs to another backend"_Str),
            rstd::move(input) });
    if (input.body.length() != input.prepared.upload().length)
        return Err(lf::Rejected<h::ExchangeInput> {
            lf::Error::Contract("upload metadata mismatch"_Str), rstd::move(input) });
    auto exchange =
        Arc<Exchange>::make(rstd::move(input), state_->options.window, state_->options.head_limit);
    exchange->fields.lock().unwrap()->trailers_available = false;
    auto dispatch                                        = state_->dispatch.operation();
    exchange->wake = Callback<void()>([dispatch = dispatch.clone()] {
        dispatch.notify();
    });
    return Ok(lf::PreparedOperation<h::ResponseMessage>::make(
        Driver { state_.clone(), rstd::move(exchange), rstd::move(dispatch) }));
}
void Backend::request_close() const { stop(state_); }
auto Backend::close() const -> coro<lf::Result<empty>> {
    request_close();
    return registration_->release();
}
} // namespace ncrequest::client::qt_network
