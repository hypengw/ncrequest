export module ncrequest:upgrade;
export import lifetchto.http;
export namespace ncrequest
{
using UpgradeRequest    = lifetchto::http::UpgradeRequest;
using UpgradeOptions    = lifetchto::http::UpgradeOptions;
using UpgradeFailure    = lifetchto::http::UpgradeFailure;
using UpgradeConnection = lifetchto::http::UpgradeConnection;
using PendingUpgrade    = lifetchto::http::PendingUpgrade;
} // namespace ncrequest
