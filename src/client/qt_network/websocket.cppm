module;
#include <memory_resource>

export module ncrequest:client_qt_network_websocket;
export import :qt;
export import ncrequest.type;
export import :client_callback;

namespace ncrequest::client::qt_network
{

using namespace ncrequest::qt;

export class WebSocketBackend : public NoCopy {
public:
    constexpr static u64 MaxBufferSize { 16 * 1024 };
    using ConnectedCallback    = client::Callback<void()>;
    using DisconnectedCallback = client::Callback<void()>;
    using MessageCallback      = client::Callback<void(slice<byte>, bool last)>;
    using ErrorCallback        = client::Callback<void(rstd::ref<rstd::str>)>;

    explicit WebSocketBackend(
        QObject* parent = nullptr, rstd::Option<u64> max_buffer_size = None(),
        std::pmr::memory_resource* mem_pool = std::pmr::get_default_resource())
        : m_owned_socket(
              parent == nullptr
                  ? Some(Box<QWebSocket>::make(QString {}, QWebSocketProtocol::VersionLatest))
                  : None<Box<QWebSocket>>()),
          m_socket(parent == nullptr
                       ? (*m_owned_socket).get()
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

    auto connect(ref<str> url) -> rstd::async::Completion<bool> {
        auto made       = rstd::async::Completion<bool>::make();
        auto pair       = rstd::move(made).unwrap();
        auto completion = rstd::move(pair.get<0>());
        auto handle     = rstd::move(pair.get<1>());
        auto* socket    = m_socket.data();

        if (QCoreApplication::instance() == nullptr) {
            (void)handle.complete(false);
            emit_error(QString::fromUtf8("Qt WebSockets backend requires a QCoreApplication"));
            return completion;
        }
        if (socket == nullptr) {
            (void)handle.complete(false);
            emit_error(QString::fromUtf8("QWebSocket is not available"));
            return completion;
        }
        if (socket->thread() != QThread::currentThread()) {
            (void)handle.complete(false);
            emit_error(QString::fromUtf8("QWebSocket must be used from its owner thread"));
            return completion;
        }
        if (m_connected) {
            (void)handle.complete(true);
            return completion;
        }
        if (m_connecting) {
            (void)handle.complete(false);
            return completion;
        }

        m_connecting      = true;
        m_connect_promise = Some(rstd::move(handle));
        socket->open(QUrl(QString::fromUtf8(reinterpret_cast<const char*>(url.data()),
                                           static_cast<qsizetype>(url.size()))));
        return completion;
    }

    void disconnect() {
        auto* socket = m_socket.data();
        if (socket == nullptr) return;
        if (socket->state() == QAbstractSocket::UnconnectedState) return;
        socket->close();
    }

    bool is_connected() const { return m_connected && m_socket != nullptr; }

    void send(ref<str> message) {
        send(slice<byte>::from_raw_parts(
            reinterpret_cast<const byte*>(message.data()), message.size()));
    }

    void send(slice<byte> message) {
        auto* socket = m_socket.data();
        if (! m_connected || socket == nullptr) return;
        socket->sendBinaryMessage(
            QByteArray(reinterpret_cast<const char*>(message.as_raw_ptr()),
                       static_cast<qsizetype>(message.len())));
    }

    void set_on_connected_callback(ConnectedCallback callback) {
        m_on_connected = rstd::move(callback);
    }

    void set_on_disconnected_callback(DisconnectedCallback callback) {
        m_on_disconnected = rstd::move(callback);
    }

    void set_on_message_callback(MessageCallback callback) { m_on_message = rstd::move(callback); }

    void set_on_error_callback(ErrorCallback callback) { m_on_error = rstd::move(callback); }

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
            if (m_on_disconnected) m_on_disconnected();
        }));

        m_connections.append(QObject::connect(
            socket, &QWebSocket::textMessageReceived, socket, [this](const QString& message) {
                if (! m_on_message) return;
                auto bytes = message.toUtf8();
                m_on_message(slice<byte>::from_raw_parts(
                                 reinterpret_cast<const byte*>(bytes.constData()),
                                 static_cast<usize>(bytes.size())),
                             true);
            }));

        m_connections.append(QObject::connect(
            socket, &QWebSocket::binaryMessageReceived, socket, [this](const QByteArray& message) {
                if (! m_on_message) return;
                m_on_message(slice<byte>::from_raw_parts(
                                 reinterpret_cast<const byte*>(message.constData()),
                                 static_cast<usize>(message.size())),
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
        auto promise = m_connect_promise.take();
        if (promise.is_none()) return;
        (void)(*promise).complete(success);
    }

    void emit_error(const QString& message) {
        if (! m_on_error) return;
        auto text = message.toUtf8();
        m_on_error(ref<str>::from_raw_parts(
            reinterpret_cast<const u8*>(text.constData()), static_cast<usize>(text.size())));
    }

    void disconnect_signals() {
        for (auto const& connection : m_connections) {
            QObject::disconnect(connection);
        }
        m_connections.clear();
    }

    Option<Box<QWebSocket>>        m_owned_socket;
    QPointer<QWebSocket>           m_socket;
    QList<QMetaObject::Connection> m_connections;
    bool                           m_connected;
    bool                           m_connecting;

    Option<rstd::async::CompletionHandle<bool>> m_connect_promise;
    ConnectedCallback       m_on_connected;
    DisconnectedCallback    m_on_disconnected;
    MessageCallback         m_on_message;
    ErrorCallback           m_on_error;

    std::pmr::polymorphic_allocator<rstd::byte> m_alloc;
    std::pmr::vector<rstd::byte>                m_read_buffer;
};

} // namespace ncrequest::client::qt_network
