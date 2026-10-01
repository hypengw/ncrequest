module;
#include <curl/curl.h>
#include <limits>
module ncrequest;
import :client_curl_response;
import :client_curl_session;
import :session_share_backend;
import ncrequest.coro;
import rstd.cppstd;

using namespace rstd::literals;
using rstd::async::yield_now;
using rstd::bytes::Bytes;
using rstd::bytes::BytesMut;
using rstd::ffi::CString;
using rstd::vec::Vec;
using std::pmr::polymorphic_allocator;

namespace ncrequest::client::curl
{

namespace
{

void apply_easy_request(CurlEasy& easy, const Request& req) {
    auto url_bytes = Vec<u8>::make();
    url_bytes.extend_from_slice(req.url_info().as_ref().as_bytes());
    auto url = CString::from_vec_unchecked(rstd::move(url_bytes));
    easy.setopt(CURLoption::CURLOPT_URL, url.as_ptr());
    {
        auto& timeout = req.get_opt<req_opt::Timeout>();

        easy.setopt(CURLoption::CURLOPT_LOW_SPEED_LIMIT,
                    static_cast<long>(timeout.low_speed.to_primitive()));
        easy.setopt(CURLoption::CURLOPT_LOW_SPEED_TIME,
                    static_cast<long>(timeout.transfer_timeout.to_primitive()));
        easy.setopt(CURLoption::CURLOPT_CONNECTTIMEOUT,
                    static_cast<long>(timeout.connect_timeout.to_primitive()));
    }
    {
        auto& tcp = req.get_opt<req_opt::Tcp>();
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPALIVE, tcp.keepalive);
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPIDLE,
                    static_cast<long>(tcp.keepidle.to_primitive()));
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPINTVL,
                    static_cast<long>(tcp.keepintvl.to_primitive()));
    }
    {
        auto& p = req.get_opt<req_opt::Proxy>();
        easy.setopt(CURLoption::CURLOPT_PROXYTYPE, static_cast<long>(p.type));
        easy.setopt(CURLoption::CURLOPT_PROXY, p.content.empty() ? nullptr : p.content.c_str());
    }
    {
        auto& p = req.get_opt<req_opt::SSL>();
        easy.setopt(CURLoption::CURLOPT_SSL_VERIFYPEER, (long)p.verify_certificate);
        easy.setopt(CURLoption::CURLOPT_PROXY_SSL_VERIFYPEER, (long)p.verify_certificate);
    }
    {
        auto& p = req.get_opt<req_opt::Share>();
        if (p.share) {
            easy.setopt<CURLoption::CURLOPT_SHARE>(
                detail::SessionShareAccess::curl_handle(*p.share));
        }
    }
    easy.set_header(req.header());
}

} // namespace

ResponseBackend::Inner::Inner(ResponseBackend* res, Request req, SessionBackend& ses)
    : m_q(res),
      m_finished(false),
      m_connect(Connection::make(rstd::move(req), ses.channel_rc(), ses.allocator())),
      m_allocator(ses.allocator()) {}

ResponseBackend::ResponseBackend(Request req, SessionBackend& ses) noexcept
    : m_inner(Arc<Inner>::make(this, rstd::move(req), ses)) {
    apply_easy_request(connection().easy(), request());
    auto& reader = request().get_opt<req_opt::Read>();
    if (reader.callback) connection().set_send_callback(reader.callback);
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

auto ResponseBackend::allocator() const -> const polymorphic_allocator<char>& {
    return m_inner->m_allocator;
}

Arc<ResponseBackend> ResponseBackend::make_response(Request req, SessionBackend& ses) {
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

auto ResponseBackend::prepare_perform() -> Result<rstd::empty> {
    auto&    easy   = connection().easy();
    auto&    req    = request();
    auto     method = req.method().as_ref();
    auto&    body   = req.body();
    auto&    reader = req.get_opt<req_opt::Read>();
    CURLcode code   = CURLE_OK;
    auto     set    = [&](CURLoption option, auto value) {
        if (code == CURLE_OK) code = easy.setopt(option, value);
    };
    if (method == "HEAD"_str) {
        set(CURLOPT_NOBODY, 1L);
    } else if (method == "POST"_str || body.is_some()) {
        set(CURLOPT_POST, 1L);
        if (reader.callback) {
            if (reader.size.to_primitive() > std::numeric_limits<curl_off_t>::max())
                return Err(Error::InvalidState("request body is too large"));
            set(CURLOPT_POSTFIELDS, static_cast<const char*>(nullptr));
            set(CURLOPT_POSTFIELDSIZE_LARGE,
                reader.size == usize() ? static_cast<curl_off_t>(-1)
                                       : static_cast<curl_off_t>(reader.size.to_primitive()));
        } else {
            auto size = body.is_some() ? body->size().to_primitive() : 0;
            if (size > std::numeric_limits<curl_off_t>::max())
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
        return Err(Error::Client(ClientError {
            ClientBackend::Curl, i32(static_cast<int>(code)), curl_easy_strerror(code) }));
    }
    return Ok(rstd::empty {});
}

bool ResponseBackend::is_finished() const {
    if (! m_inner) return true;
    return connection().is_finished();
}

auto ResponseBackend::header() const -> const lihttpto::Headers& {
    return connection().header().headers();
}
auto ResponseBackend::head() const -> rstd::Option<rstd::ref<lihttpto::MessageHead>> {
    return Some(rstd::ref<lihttpto::MessageHead>::from_raw_parts(&connection().header()));
}
auto ResponseBackend::trailers() const -> rstd::Option<rstd::ref<lihttpto::Headers>> {
    return connection().trailers();
}
auto ResponseBackend::code() const -> rstd::Option<i32> {
    auto status = connection().header().status_code();
    if (status.is_none()) return None();
    return Some(rstd::as_cast<i32>(*status));
}

auto ResponseBackend::connection() -> Connection& { return *(m_inner->m_connect); }
auto ResponseBackend::connection() const -> const Connection& { return *(m_inner->m_connect); }

void ResponseBackend::cancel() {
    if (! m_inner) return;
    connection().about_to_cancel();
}

auto ResponseBackend::next_chunk() -> coro<Result<rstd::Option<Bytes>>> {
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
