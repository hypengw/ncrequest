module;
#include <curl/curl.h>
#include <limits.h>
module ncrequest;
import :client.curl.connection;
import :client.curl.session_share;
import ncrequest.coro;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::async::spawn;
using rstd::async::yield_now;
using rstd::ffi::CStr;
using rstd::ffi::CString;
using rstd::path::PathBuf;
using rstd::sync::Arc;
using rstd::time::Duration;

namespace ncrequest::client::curl
{

namespace
{

auto duration_units(Duration duration, bool milliseconds) -> Result<long> {
    auto scale   = milliseconds ? u128(1000) : u128(1);
    auto divisor = milliseconds ? u128(1'000'000) : u128(1'000'000'000);
    auto nanos   = u128(duration.subsec_nanos().to_primitive());
    auto value =
        u128(duration.as_secs().to_primitive()) * scale + (nanos + divisor - u128(1)) / divisor;
    if (value > u128(LONG_MAX)) return Err(Error::InvalidState("timeout exceeds curl long range"));
    return Ok(static_cast<long>(value.to_primitive()));
}

auto apply_easy_request(CurlEasy& easy, const Request& req, const EffectiveOptions& options)
    -> Result<empty> {
    // A paused write callback is replayed whole; it must fit in an empty queue.
    if (options.limits().receive_buffer_bytes < usize(CURL_MAX_WRITE_SIZE))
        return Err(Error::Unsupported("curl receive buffer must fit CURL_MAX_WRITE_SIZE"));
    auto code = CURLcode::CURLE_OK;
    auto set  = [&](CURLoption option, auto value) {
        if (code == CURLcode::CURLE_OK) code = easy.setopt(option, value);
    };
    auto url_bytes = Vec<u8>::make();
    url_bytes.extend_from_slice(req.url_info().as_ref().as_bytes());
    auto url = CString::from_vec_unchecked(rstd::move(url_bytes));
    set(CURLoption::CURLOPT_URL, url.as_ptr());
    {
        auto& timeout = options.timeout();

        if (timeout.connect.mode() == TimeoutLimit::Mode::Disabled)
            return Err(Error::Unsupported("curl cannot disable its connect timeout"));
        auto connect    = timeout.connect.duration();
        auto total      = timeout.total.duration();
        auto connect_ms = connect.is_some() ? duration_units(*connect, true) : Result<long>(Ok(0L));
        if (connect_ms.is_err()) return Err(rstd::move(connect_ms).unwrap_err());
        auto total_ms = total.is_some() ? duration_units(*total, true) : Result<long>(Ok(0L));
        if (total_ms.is_err()) return Err(rstd::move(total_ms).unwrap_err());
        set(CURLoption::CURLOPT_CONNECTTIMEOUT_MS, connect_ms.unwrap());
        set(CURLoption::CURLOPT_TIMEOUT_MS, total_ms.unwrap());
        long speed  = 0;
        long window = 0;
        if (timeout.low_speed.is_some()) {
            auto& low = *timeout.low_speed;
            if (low.bytes_per_second > u64(LONG_MAX))
                return Err(Error::InvalidState("low-speed rate exceeds curl long range"));
            auto seconds = duration_units(low.window, false);
            if (seconds.is_err()) return Err(rstd::move(seconds).unwrap_err());
            speed  = static_cast<long>(low.bytes_per_second.to_primitive());
            window = seconds.unwrap();
        }
        set(CURLoption::CURLOPT_LOW_SPEED_LIMIT, speed);
        set(CURLoption::CURLOPT_LOW_SPEED_TIME, window);
    }
    if (options.endpoint().socket_path().is_none()) {
        auto& tcp = options.tcp();
        set(CURLoption::CURLOPT_TCP_KEEPALIVE, tcp.keepalive ? 1L : 0L);
        set(CURLoption::CURLOPT_TCP_KEEPIDLE, static_cast<long>(tcp.keepidle.to_primitive()));
        set(CURLoption::CURLOPT_TCP_KEEPINTVL, static_cast<long>(tcp.keepintvl.to_primitive()));
    }
    auto& proxy = options.proxy();
    switch (proxy.mode()) {
    case ProxyOptions::Mode::System:
        set(CURLoption::CURLOPT_PROXY, static_cast<const char*>(nullptr));
        set(CURLoption::CURLOPT_NOPROXY, static_cast<const char*>(nullptr));
        break;
    case ProxyOptions::Mode::Disabled: set(CURLoption::CURLOPT_PROXY, ""); break;
    case ProxyOptions::Mode::Explicit: {
        auto url = proxy.url().unwrap();
        auto address =
            rstd::format("{}://{}:{}", *url->scheme(), *url->host(), proxy.port().unwrap());
        auto value = CString::from_vec_unchecked(Vec<u8>::from(address.as_str().as_bytes()));
        set(CURLoption::CURLOPT_PROXY, value.as_ptr());
        set(CURLoption::CURLOPT_NOPROXY, "");
        break;
    }
    }
    {
        auto& p = options.tls();
        set(CURLoption::CURLOPT_SSL_VERIFYPEER, p.verify_peer ? 1L : 0L);
        set(CURLoption::CURLOPT_SSL_VERIFYHOST, p.verify_hostname ? 2L : 0L);
        set(CURLoption::CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
        set(CURLoption::CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
        auto set_path = [&](CURLoption option, const PathBuf& path) {
            auto value = CString::from_vec_unchecked(
                Vec<u8>::from(path.as_path().as_os_str().as_encoded_bytes()));
            set(option, value.as_ptr());
        };
        if (p.ca_bundle.is_some()) {
            set_path(CURLoption::CURLOPT_CAINFO, *p.ca_bundle);
            set(CURLoption::CURLOPT_CAPATH, static_cast<const char*>(nullptr));
        }
        if (p.client_identity.is_some()) {
            auto* version = curl_version_info(CURLVERSION_NOW);
            if (version != nullptr && version->ssl_version != nullptr) {
                auto backend = CStr::from_ptr(version->ssl_version).to_str();
                if (backend.is_ok() && backend.unwrap().starts_with("Schannel"_str))
                    return Err(
                        Error::Unsupported("curl Schannel does not support PEM private key files"));
            }
            set(CURLoption::CURLOPT_SSLCERTTYPE, "PEM");
            set(CURLoption::CURLOPT_SSLKEYTYPE, "PEM");
            set_path(CURLoption::CURLOPT_SSLCERT, p.client_identity->certificate);
            set_path(CURLoption::CURLOPT_SSLKEY, p.client_identity->private_key);
        }
    }
    set(CURLoption::CURLOPT_FOLLOWLOCATION, 0L);
    {
        auto& p = options.share();
        if (p.share && code == CURLcode::CURLE_OK) {
            code = easy.setopt<CURLoption::CURLOPT_SHARE>(
                detail::SessionShareAccess::curl_handle(*p.share));
        }
    }
    if (code != CURLcode::CURLE_OK) return Err(rstd::into<Error>(code));
    code = easy.set_header(req.header());
    if (code != CURLcode::CURLE_OK) return Err(rstd::into<Error>(code));
    return Ok(empty {});
}

} // namespace

auto Connection::prepare() -> Result<empty> {
    create_easy();
    if (easy().status() != CURLE_OK) return Err(rstd::into<Error>(easy().status()));
    auto configured = apply_easy_request(easy(), request(), options());
    if (configured.is_err()) return Err(rstd::move(configured).unwrap_err());
    auto&    easy   = this->easy();
    auto&    req    = request();
    auto     method = req.method().as_ref();
    auto&    body   = req.body().bytes();
    auto&    reader = req.body().reader();
    auto&    stream = req.body().stream();
    CURLcode code   = CURLE_OK;
    auto     set    = [&](CURLoption option, auto value) {
        if (code == CURLE_OK) code = easy.setopt(option, value);
    };
    auto socket = options().endpoint().socket_path();
    if (socket.is_some()) {
        auto* version = curl_version_info(CURLVERSION_NOW);
        if (version == nullptr || ! (version->features & CURL_VERSION_UNIX_SOCKETS))
            return Err(Error::Unsupported("curl does not support Unix socket endpoints"));
        auto bytes = Vec<u8>::from(socket->as_os_str().as_encoded_bytes());
        auto path  = CString::from_vec_unchecked(rstd::move(bytes));
        set(CURLOPT_UNIX_SOCKET_PATH, path.as_ptr());
    }
    if (method == "HEAD"_str) {
        set(CURLOPT_NOBODY, 1L);
    } else if (stream.is_some()) {
        if (stream->size.is_some() && *stream->size > u64(i64::MAX.to_primitive()))
            return Err(Error::InvalidState("request body exceeds curl size range"));
        set(CURLOPT_UPLOAD, 1L);
        set(CURLOPT_INFILESIZE_LARGE,
            stream->size.is_some() ? static_cast<curl_off_t>(stream->size->to_primitive())
                                   : static_cast<curl_off_t>(-1));
    } else if (method == "POST"_str || body.is_some()) {
        set(CURLOPT_POST, 1L);
        if (reader.is_some()) {
            if (reader->size.is_some() && reader->size->to_primitive() > i64::MAX.to_primitive())
                return Err(Error::InvalidState("request body is too large"));
            set(CURLOPT_POSTFIELDS, static_cast<const char*>(nullptr));
            set(CURLOPT_POSTFIELDSIZE_LARGE,
                reader->size.is_none() ? static_cast<curl_off_t>(-1)
                                       : static_cast<curl_off_t>(reader->size->to_primitive()));
        } else {
            auto size = body.is_some() ? body->size().to_primitive() : 0;
            if (size > i64::MAX.to_primitive())
                return Err(Error::InvalidState("request body is too large"));
            set(CURLOPT_POSTFIELDS, size ? reinterpret_cast<const char*>(body->data()) : "");
            set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
        }
    } else {
        set(CURLOPT_HTTPGET, 1L);
    }
    if (stream.is_some() ||
        (method != "POST"_str && method != "HEAD"_str && (method != "GET"_str || body.is_some()))) {
        auto bytes = Vec<u8>::make();
        bytes.extend_from_slice(method.as_bytes());
        auto token = CString::from_vec_unchecked(rstd::move(bytes));
        set(CURLOPT_CUSTOMREQUEST, token.as_ptr());
    }
    if (code != CURLE_OK) {
        return Err(rstd::into<Error>(code));
    }
    return Ok(empty {});
}

static auto pump_upload(Arc<Connection> connection, BodyStream::Callback next, Option<u64> length)
    -> coro<void> {
    u64 count {};
    while (length.is_none() || count < *length) {
        if (connection->is_finished()) co_return;
        auto part = co_await next();
        if (part.is_err()) {
            connection->fail_upload(rstd::move(part).unwrap_err());
            co_return;
        }
        if (part->is_none()) {
            if (length.is_some()) {
                connection->fail_upload(
                    Error::Protocol(ProtocolError::BodyLengthMismatch, nullptr));
                co_return;
            }
            break;
        }
        auto bytes = part->take().unwrap();
        auto size  = u64(bytes.size().to_primitive());
        if (size > u64::MAX - count || (length.is_some() && size > *length - count)) {
            connection->fail_upload(Error::Protocol(ProtocolError::BodyLengthMismatch, nullptr));
            co_return;
        }
        count += size;
        while (bytes.size() != usize()) {
            auto written = co_await connection->write_some(bytes);
            if (written.error.is_some()) {
                connection->fail_upload(written.error.take().unwrap());
                co_return;
            }
            if (written.eof) co_return;
        }
        co_await yield_now();
    }
    connection->finish_upload();
}

void Connection::start_upload() {
    auto& stream = request().body().stream();
    if (stream.is_none()) return;
    auto task = spawn(pump_upload(get_arc(), stream->next.clone(), stream->size));
    {
        auto lock = RawMutexGuard { m_mutex };
        if (m_state != State::Finished && m_state != State::Canceled) {
            m_upload_task = Some(rstd::move(task));
            return;
        }
    }
    task.abort();
}

} // namespace ncrequest::client::curl
