export module ncrequest:proxy;
export import lifetchto.http;
export namespace ncrequest
{
using ProxyOptions      = lifetchto::http::ProxyOptions;
using ProxyEnvironment  = lifetchto::http::ProxyEnvironment;
using ProxyType         = lifetchto::http::ProxyType;
using ProxyCapabilities = lifetchto::http::ProxyCapabilities;
} // namespace ncrequest
