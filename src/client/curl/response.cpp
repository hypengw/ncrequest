module;
#include <curl/curl.h>
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
using rstd::ffi::CString;
using rstd::sync::Arc;

namespace ncrequest::client::curl
{

namespace
{

void apply_easy_request(CurlEasy& easy, const Request& req, const EffectiveOptions& options) {
    auto url_bytes = Vec<u8>::make();
    url_bytes.extend_from_slice(req.url_info().as_ref().as_bytes());
    auto url = CString::from_vec_unchecked(rstd::move(url_bytes));
    easy.setopt(CURLoption::CURLOPT_URL, url.as_ptr());
    {
        auto& timeout = options.timeout();

        easy.setopt(CURLoption::CURLOPT_LOW_SPEED_LIMIT,
                    static_cast<long>(timeout.low_speed.to_primitive()));
        easy.setopt(CURLoption::CURLOPT_LOW_SPEED_TIME,
                    static_cast<long>(timeout.transfer_timeout.to_primitive()));
        easy.setopt(CURLoption::CURLOPT_CONNECTTIMEOUT,
                    static_cast<long>(timeout.connect_timeout.to_primitive()));
    }
    if (options.endpoint().socket_path().is_none()) {
        auto& tcp = options.tcp();
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPALIVE, tcp.keepalive ? 1L : 0L);
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPIDLE,
                    static_cast<long>(tcp.keepidle.to_primitive()));
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPINTVL,
                    static_cast<long>(tcp.keepintvl.to_primitive()));
    }
    if (options.endpoint().socket_path().is_none()) {
        auto& p = options.proxy();
        easy.setopt(CURLoption::CURLOPT_PROXYTYPE, static_cast<long>(p.type));
        auto proxy = CString::from_vec_unchecked(Vec<u8>::from(p.content.as_str().as_bytes()));
        easy.setopt(CURLoption::CURLOPT_PROXY, p.content.is_empty() ? nullptr : proxy.as_ptr());
    }
    {
        auto& p = options.tls();
        easy.setopt(CURLoption::CURLOPT_SSL_VERIFYPEER, (long)p.verify_certificate);
        easy.setopt(CURLoption::CURLOPT_PROXY_SSL_VERIFYPEER, (long)p.verify_certificate);
    }
    {
        auto& p = options.share();
        if (p.share) {
            easy.setopt<CURLoption::CURLOPT_SHARE>(
                detail::SessionShareAccess::curl_handle(*p.share));
        }
    }
    easy.set_header(req.header());
}

} // namespace

ResponseBackend::Inner::Inner(ResponseBackend* res, PreparedRequest req, SessionBackend& ses)
    : m_q(res), m_finished(false), m_connect(Connection::make(rstd::move(req), ses.channel_rc())) {}

ResponseBackend::ResponseBackend(PreparedRequest req, SessionBackend& ses) noexcept
    : m_inner(Arc<Inner>::make(this, rstd::move(req), ses)) {
    apply_easy_request(connection().easy(), request(), connection().options());
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
    connection().send_action(pause ? Connection::Action::PauseSend
                                   : Connection::Action::UnPauseSend);
    return true;
}
bool ResponseBackend::pause_recv(bool pause) {
    connection().send_action(pause ? Connection::Action::PauseRecv
                                   : Connection::Action::UnPauseRecv);
    return true;
}

auto ResponseBackend::prepare_perform() -> Result<empty> {
    auto&    easy   = connection().easy();
    auto&    req    = request();
    auto     method = req.method().as_ref();
    auto&    body   = req.body().bytes();
    auto&    reader = req.body().reader();
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
        set(CURLOPT_PROXY, "");
    }
    if (method == "HEAD"_str) {
        set(CURLOPT_NOBODY, 1L);
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
    if (method != "POST"_str && method != "HEAD"_str && (method != "GET"_str || body.is_some())) {
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
