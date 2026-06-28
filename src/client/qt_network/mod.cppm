export module ncrequest.qt_network;
export import ncrequest;

namespace ncrequest::qt_network
{

export using SessionBackend  = client::qt_network::SessionBackend;
export using ResponseBackend = client::qt_network::ResponseBackend;
export using Session         = ncrequest::Session;
export using Response        = ncrequest::Response;
export using Options         = client::qt_network::Options;

} // namespace ncrequest::qt_network
