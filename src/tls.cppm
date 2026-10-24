export module ncrequest:tls;
export import lifetchto.http;
export namespace ncrequest
{
using TlsOptions      = lifetchto::http::TlsOptions;
using TlsIdentity     = lifetchto::http::TlsIdentity;
using TlsVerification = lifetchto::http::TlsVerification;
using TlsProfile      = lifetchto::http::TlsProfile;
using TcpOptions      = lifetchto::http::TcpOptions;
using LowSpeedOptions = lifetchto::http::LowSpeedOptions;
} // namespace ncrequest
