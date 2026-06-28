export module ncrequest;
export import :request;
export import :response;
export import :session;
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
export import :client_qt_network;
#endif
#ifdef NCREQUEST_WEBSOCKET_BACKEND_QT_WEBSOCKETS
export import :client_qt_websockets;
#endif
export import :websocket;
