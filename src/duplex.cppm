export module ncrequest:duplex;
export import lifetchto.http;
export namespace ncrequest
{
using Duplex        = lifetchto::http::DuplexConnection;
using PendingDuplex = lifetchto::http::PendingDuplex;
using DuplexRequest = lifetchto::http::DuplexRequest;
using DuplexOptions = lifetchto::http::DuplexOptions;
} // namespace ncrequest
