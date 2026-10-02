module;
#include <curl/curl.h>
#include <limits.h>
module ncrequest;
import :client_curl_response;
import :client_curl_session;
import :session_share_backend;
import ncrequest.coro;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::async::yield_now;
using rstd::bytes::Bytes;
using rstd::bytes::BytesMut;
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
        auto address = proxy.url().unwrap()->as_ref();
        auto value   = CString::from_vec_unchecked(Vec<u8>::from(address.as_bytes()));
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
    easy.set_header(req.header());
    return Ok(empty {});
}

} // namespace

ResponseBackend::Inner::Inner(ResponseBackend* res, PreparedRequest req, SessionBackend& ses)
    : m_q(res), m_finished(false), m_connect(Connection::make(rstd::move(req), ses.channel_rc())) {}

ResponseBackend::ResponseBackend(PreparedRequest req, SessionBackend& ses) noexcept
    : m_inner(Arc<Inner>::make(this, rstd::move(req), ses)) {
    auto& reader = request().body().reader();
    if (reader.is_some()) connection().set_send_callback(reader->callback.clone());
}

ResponseBackend::ResponseBackend(ResponseBackend&& other) noexcept
    : m_inner(rstd::move(other.m_inner)) {
    if (m_inner) {
        m_inner->m_q = this;
    }
}

ResponseBackend& ResponseBackend::operator=(ResponseBackend&& other) noexcept {
    if (this == &other) return *this;

    cancel();
    m_inner = rstd::move(other.m_inner);
    if (m_inner) {
        m_inner->m_q = this;
    }
    return *this;
}

ResponseBackend::~ResponseBackend() noexcept { cancel(); }

Arc<ResponseBackend> ResponseBackend::make_response(PreparedRequest req, SessionBackend& ses) {
    return Arc<ResponseBackend>::make(rstd::move(req), ses);
}

const Request& ResponseBackend::request() const { return connection().request(); }

bool ResponseBackend::pause_send(bool pause) {
    connection().request_pause(false, pause);
    return true;
}
bool ResponseBackend::pause_recv(bool pause) {
    connection().request_pause(true, pause);
    return true;
}

auto ResponseBackend::prepare_perform() -> Result<empty> {
    auto configured = apply_easy_request(connection().easy(), request(), connection().options());
    if (configured.is_err()) return Err(rstd::move(configured).unwrap_err());
    auto&    easy   = connection().easy();
    auto&    req    = request();
    auto     method = req.method().as_ref();
    auto&    body   = req.body().bytes();
    auto&    reader = req.body().reader();
    auto&    stream = req.body().stream();
    CURLcode code   = CURLE_OK;
    auto     set    = [&](CURLoption option, auto value) {
        if (code == CURLE_OK) code = easy.setopt(option, value);
    };
    auto socket = connection().options().endpoint().socket_path();
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

bool ResponseBackend::is_finished() const {
    if (! m_inner) return true;
    return connection().is_finished();
}

auto ResponseBackend::header() const -> const lihttpto::Headers& {
    return connection().header().headers();
}
auto ResponseBackend::head() const -> Option<ref<lihttpto::MessageHead>> {
    return Some(ref<lihttpto::MessageHead>::from_raw_parts(&connection().header()));
}
auto ResponseBackend::trailers() const -> Option<ref<lihttpto::Headers>> {
    return connection().trailers();
}
auto ResponseBackend::code() const -> Option<i32> {
    auto status = connection().header().status_code();
    if (status.is_none()) return None();
    return Some(as_cast<i32>(*status));
}

auto ResponseBackend::connection() -> Connection& { return *(m_inner->m_connect); }
auto ResponseBackend::connection() const -> const Connection& { return *(m_inner->m_connect); }

void ResponseBackend::cancel() {
    if (! m_inner) return;
    connection().about_to_cancel();
}

auto ResponseBackend::next_chunk() -> coro<Result<Option<Bytes>>> {
    auto chunk = BytesMut::with_capacity(ReadSize);
    for (;;) {
        auto read = co_await connection().read_some(chunk);
        if (read.error.is_some()) co_return Err(read.error.take().unwrap());
        if (read.eof) co_return Ok(None<Bytes>());
        if (read.size != usize()) co_return Ok(Some(chunk.freeze()));
        co_await yield_now();
    }
}

} // namespace ncrequest::client::curl
