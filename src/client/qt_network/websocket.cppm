module;
#include <QSslConfiguration>
#include <QNetworkProxy>
#include <QObject>
module ncrequest:client.qt_network.websocket;
import :websocket_backend;
import :client.qt_network.dispatch;
using namespace rstd::prelude;
namespace ncrequest::client::qt_network
{
struct WebSocketServiceState;
class WebSocketService {
    Arc<WebSocketServiceState> state_;

public:
    WebSocketService(QSslConfiguration, QNetworkProxy, lifetchto::http::TlsVerification, Dispatch);
    ~WebSocketService();
    WebSocketService(const WebSocketService&) = delete;
    auto backend() const -> Arc<rstd::dyn<ncrequest::WebSocketBackend>>;
    void advance(QObject*, slice<usize>);
    void stop() const;
    void dispose();
    auto empty() const -> bool;
};
} // namespace ncrequest::client::qt_network
