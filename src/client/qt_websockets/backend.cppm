module;
#include <functional>
#include <future>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketProtocol>

export module ncrequest:client_qt_websockets;
export import ncrequest.type;

namespace ncrequest::client::qt_websockets
{

export class WebSocketBackend : public NoCopy {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 };
    using ConnectedCallback = std::function<void()>;
    using MessageCallback   = std::function<void(std::span<const rstd::byte>, bool last)>;
    using ErrorCallback     = std::function<void(rstd::ref<rstd::str>)>;

    explicit WebSocketBackend(QObject* parent = nullptr, rstd::Option<u64> max_buffer_size = None(),
                              std::pmr::memory_resource* mem_pool = std::pmr::get_default_resource())
        : m_owned_socket(parent == nullptr ? std::make_unique<QWebSocket>(
                                                 QString {}, QWebSocketProtocol::VersionLatest)
                                           : nullptr),
          m_socket(parent == nullptr
                       ? m_owned_socket.get()
                       : new QWebSocket(QString {}, QWebSocketProtocol::VersionLatest, parent)),
          m_connected(false),
          m_connecting(false),
          m_alloc(mem_pool),
          m_read_buffer(max_buffer_size.unwrap_or(MaxBufferSize), m_alloc) {
        bind_socket();
    }

    ~WebSocketBackend() {
        disconnect();
        disconnect_signals();
    }

    WebSocketBackend(const WebSocketBackend&)            = delete;
    WebSocketBackend& operator=(const WebSocketBackend&) = delete;

    auto connect(const std::string& url) -> std::future<bool> {
        auto  promise = make_arc<std::promise<bool>>();
        auto  future  = promise->get_future();
        auto* socket  = m_socket.data();

        if (QCoreApplication::instance() == nullptr) {
            promise->set_value(false);
            emit_error(QString::fromUtf8("Qt WebSockets backend requires a QCoreApplication"));
            return future;
        }
        if (socket == nullptr) {
            promise->set_value(false);
            emit_error(QString::fromUtf8("QWebSocket is not available"));
            return future;
        }
        if (socket->thread() != QThread::currentThread()) {
            promise->set_value(false);
            emit_error(QString::fromUtf8("QWebSocket must be used from its owner thread"));
            return future;
        }
        if (m_connected) {
            promise->set_value(true);
            return future;
        }
        if (m_connecting) {
            promise->set_value(false);
            return future;
        }

        m_connecting      = true;
        m_connect_promise = promise;
        socket->open(QUrl(QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()))));
        return future;
    }

    void disconnect() {
        auto* socket = m_socket.data();
        if (socket == nullptr) return;
        if (socket->state() == QAbstractSocket::UnconnectedState) return;
        socket->close();
    }

    bool is_connected() const { return m_connected && m_socket != nullptr; }

    void send(std::string_view message) {
        send(std::span<const rstd::byte> { reinterpret_cast<const rstd::byte*>(message.data()),
                                           message.size() });
    }

    void send(std::span<const rstd::byte> message) {
        auto* socket = m_socket.data();
        if (! m_connected || socket == nullptr) return;
        socket->sendBinaryMessage(QByteArray(reinterpret_cast<const char*>(message.data()),
                                             static_cast<qsizetype>(message.size())));
    }

    void set_on_connected_callback(ConnectedCallback callback) {
        m_on_connected = rstd::move(callback);
    }

    void set_on_message_callback(MessageCallback callback) { m_on_message = rstd::move(callback); }

    void set_on_error_callback(ErrorCallback callback) { m_on_error = rstd::move(callback); }

    auto on_message_callback() -> const MessageCallback& { return m_on_message; }

private:
    void bind_socket() {
        auto* socket = m_socket.data();
        if (socket == nullptr) return;

        m_connections.append(QObject::connect(socket, &QWebSocket::connected, socket, [this] {
            m_connected  = true;
            m_connecting = false;
            complete_connect(true);
            if (m_on_connected) m_on_connected();
        }));

        m_connections.append(QObject::connect(socket, &QWebSocket::disconnected, socket, [this] {
            m_connected = false;
            if (m_connecting) {
                m_connecting = false;
                complete_connect(false);
            }
        }));

        m_connections.append(QObject::connect(
            socket, &QWebSocket::textMessageReceived, socket, [this](const QString& message) {
                if (! m_on_message) return;
                auto bytes = message.toUtf8();
                m_on_message(std::span<const rstd::byte> { reinterpret_cast<const rstd::byte*>(
                                                               bytes.constData()),
                                                           static_cast<usize>(bytes.size()) },
                             true);
            }));

        m_connections.append(QObject::connect(
            socket, &QWebSocket::binaryMessageReceived, socket, [this](const QByteArray& message) {
                if (! m_on_message) return;
                m_on_message(std::span<const rstd::byte> { reinterpret_cast<const rstd::byte*>(
                                                               message.constData()),
                                                           static_cast<usize>(message.size()) },
                             true);
            }));

        m_connections.append(QObject::connect(
            socket, &QWebSocket::errorOccurred, socket, [this](QAbstractSocket::SocketError) {
                if (m_connecting) {
                    m_connecting = false;
                    complete_connect(false);
                }
                auto* local = m_socket.data();
                emit_error(local == nullptr ? QString {} : local->errorString());
            }));

        m_connections.append(QObject::connect(socket, &QObject::destroyed, socket, [this] {
            m_connected  = false;
            m_connecting = false;
            complete_connect(false);
            m_socket = nullptr;
        }));
    }

    void complete_connect(bool success) {
        auto promise = rstd::move(m_connect_promise);
        if (promise == nullptr) return;
        promise->set_value(success);
    }

    void emit_error(const QString& message) {
        if (! m_on_error) return;
        auto text = message.toStdString();
        m_on_error(text);
    }

    void disconnect_signals() {
        for (auto const& connection : m_connections) {
            QObject::disconnect(connection);
        }
        m_connections.clear();
    }

    std::unique_ptr<QWebSocket>    m_owned_socket;
    QPointer<QWebSocket>           m_socket;
    QList<QMetaObject::Connection> m_connections;
    bool                           m_connected;
    bool                           m_connecting;

    Arc<std::promise<bool>> m_connect_promise;
    ConnectedCallback       m_on_connected;
    MessageCallback         m_on_message;
    ErrorCallback           m_on_error;

    std::pmr::polymorphic_allocator<rstd::byte> m_alloc;
    std::pmr::vector<rstd::byte>                m_read_buffer;
};

} // namespace ncrequest::client::qt_websockets
