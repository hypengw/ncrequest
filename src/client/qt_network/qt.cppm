module;
#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QIODevice>
#include <QHttpHeaders>
#include <QList>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketProtocol>

export module ncrequest:qt;

export namespace ncrequest::qt
{
using ::qint64;
using ::qsizetype;
using ::quint16;

using ::QAbstractSocket;
using ::QByteArray;
using ::QCoreApplication;
using ::QDateTime;
using ::QIODevice;
using ::QHttpHeaders;
using ::QList;
using ::QMetaObject;
using ::QNetworkAccessManager;
using ::QNetworkCookie;
using ::QNetworkCookieJar;
using ::QNetworkProxy;
using ::QNetworkReply;
using ::QNetworkRequest;
using ::QObject;
using ::QPointer;
using ::QSslConfiguration;
using ::QSslSocket;
using ::QString;
using ::QThread;
using ::QUrl;
using ::QWebSocket;

namespace Qt
{
using ConnectionType = ::Qt::ConnectionType;

inline constexpr auto QueuedConnection         = ::Qt::QueuedConnection;
inline constexpr auto BlockingQueuedConnection = ::Qt::BlockingQueuedConnection;
} // namespace Qt

namespace QWebSocketProtocol
{
using Version   = ::QWebSocketProtocol::Version;
using CloseCode = ::QWebSocketProtocol::CloseCode;

inline constexpr auto VersionUnknown         = ::QWebSocketProtocol::VersionUnknown;
inline constexpr auto Version0               = ::QWebSocketProtocol::Version0;
inline constexpr auto Version4               = ::QWebSocketProtocol::Version4;
inline constexpr auto Version5               = ::QWebSocketProtocol::Version5;
inline constexpr auto Version6               = ::QWebSocketProtocol::Version6;
inline constexpr auto Version7               = ::QWebSocketProtocol::Version7;
inline constexpr auto Version8               = ::QWebSocketProtocol::Version8;
inline constexpr auto Version13              = ::QWebSocketProtocol::Version13;
inline constexpr auto VersionLatest          = ::QWebSocketProtocol::VersionLatest;
inline constexpr auto CloseCodeNormal        = ::QWebSocketProtocol::CloseCodeNormal;
inline constexpr auto CloseCodeGoingAway     = ::QWebSocketProtocol::CloseCodeGoingAway;
inline constexpr auto CloseCodeProtocolError = ::QWebSocketProtocol::CloseCodeProtocolError;
inline constexpr auto CloseCodeDatatypeNotSupported =
    ::QWebSocketProtocol::CloseCodeDatatypeNotSupported;
inline constexpr auto CloseCodeReserved1004      = ::QWebSocketProtocol::CloseCodeReserved1004;
inline constexpr auto CloseCodeMissingStatusCode = ::QWebSocketProtocol::CloseCodeMissingStatusCode;
inline constexpr auto CloseCodeAbnormalDisconnection =
    ::QWebSocketProtocol::CloseCodeAbnormalDisconnection;
inline constexpr auto CloseCodeWrongDatatype    = ::QWebSocketProtocol::CloseCodeWrongDatatype;
inline constexpr auto CloseCodePolicyViolated   = ::QWebSocketProtocol::CloseCodePolicyViolated;
inline constexpr auto CloseCodeTooMuchData      = ::QWebSocketProtocol::CloseCodeTooMuchData;
inline constexpr auto CloseCodeMissingExtension = ::QWebSocketProtocol::CloseCodeMissingExtension;
inline constexpr auto CloseCodeBadOperation     = ::QWebSocketProtocol::CloseCodeBadOperation;
inline constexpr auto CloseCodeTlsHandshakeFailed =
    ::QWebSocketProtocol::CloseCodeTlsHandshakeFailed;
} // namespace QWebSocketProtocol
} // namespace ncrequest::qt
