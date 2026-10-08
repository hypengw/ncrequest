export module ncrequest:websocket_connection;
export import :upgrade;
export import liwebsocketo;
using namespace rstd::prelude;

export namespace ncrequest
{
using WebSocketError = liwebsocketo::WebSocketError;
template<class T>
using WebSocketResult = liwebsocketo::WebSocketResult<T>;
struct WebSocketOptions : liwebsocketo::WebSocketOptions {
    UpgradeOptions upgrade;
};
struct WebSocketConnectFailure {
    Option<UpgradeFailure>               upgrade;
    Option<liwebsocketo::HandshakeError> handshake;
    Option<WebSocketError>               connection;
};
class WebSocketConnection : public liwebsocketo::WebSocketConnection<UpgradeConnection> {
    using Driver = liwebsocketo::WebSocketConnection<UpgradeConnection>;
    explicit WebSocketConnection(Driver driver): Driver(rstd::move(driver)) {}

public:
    // Uses http/https transport URLs; no redirects or proxy inheritance.
    static auto connect(lihttpto::Url url, lihttpto::Headers headers = {},
                        Vec<String> protocols = {}, WebSocketOptions options = {})
        -> rstd::async::coro<rstd::Result<WebSocketConnection, WebSocketConnectFailure>> {
        if (url.authority().is_none() || options.receive_messages == usize() ||
            options.receive_bytes == usize())
            co_return Err(WebSocketConnectFailure { .connection = Some(WebSocketError::Protocol) });
        array<u8, 16> nonce;
        if (rstd::random::fill_secure(nonce.as_mut_slice()).is_err())
            co_return Err(WebSocketConnectFailure { .connection = Some(WebSocketError::Random) });
        auto handshake = liwebsocketo::ClientHandshake::make(url.request_target().as_str(),
                                                             *url.authority(),
                                                             nonce,
                                                             rstd::move(headers),
                                                             rstd::move(protocols));
        if (handshake.is_err())
            co_return Err(WebSocketConnectFailure { .handshake = Some(handshake.unwrap_err()) });
        auto opened = co_await UpgradeConnection::open_prepared(
            rstd::move(url), handshake->request(), options.upgrade);
        if (opened.is_err())
            co_return Err(
                WebSocketConnectFailure { .upgrade = Some(rstd::move(opened).unwrap_err()) });
        auto wire      = rstd::move(opened).unwrap();
        auto validated = handshake->validate(wire.response());
        if (validated.is_err())
            co_return Err(WebSocketConnectFailure { .handshake = Some(validated.unwrap_err()) });
        auto driver = Driver::start(
            rstd::move(wire), liwebsocketo::Role::Client, options, rstd::move(validated).unwrap());
        if (driver.is_err())
            co_return Err(WebSocketConnectFailure { .connection = Some(driver.unwrap_err()) });
        co_return Ok(WebSocketConnection(rstd::move(driver).unwrap()));
    }
};
} // namespace ncrequest
