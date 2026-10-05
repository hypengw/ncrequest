export module ncrequest.qt_network;
export import ncrequest;

namespace ncrequest::qt_network
{

export using SessionBackend   = client::qt_network::SessionBackend;
export using ResponseBackend  = client::qt_network::ResponseBackend;
export using WebSocketBackend = client::qt_network::WebSocketBackend;
export using Session          = ncrequest::Session;
export using Response         = ncrequest::Response;
export using WebSocketClient  = ncrequest::WebSocketClient;
export using Options          = client::qt_network::Options;

} // namespace ncrequest::qt_network
