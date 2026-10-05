export module ncrequest;
export import :request;
export import :response;
export import :session;
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
export import :client_qt_network;
#endif
export import :websocket;
#if defined(NCREQUEST_CLIENT_BACKEND_CURL) && defined(__linux__)
export import :unix_duplex;
export import :upgrade;
#endif
