module;
module ncrequest;
import :client_curl_response;
import :client_curl_session;
import :session_share_backend;
import ncrequest.coro;

namespace ncrequest::client::curl
{

namespace
{

void apply_easy_request(ResponseBackend::Inner* rsp, CurlEasy& easy, const Request& req) {
    auto url_bytes = rstd::vec::Vec<u8>::make();
    url_bytes.extend_from_slice(rstd::str_::as_bytes(ref<str>(req.url())));
    auto url = rstd::ffi::CString::from_vec_unchecked(rstd::move(url_bytes));
    easy.setopt(CURLoption::CURLOPT_URL,
                reinterpret_cast<const char*>(url.to_bytes_with_nul().as_raw_ptr()));
    {
        auto& timeout = req.get_opt<req_opt::Timeout>();

        easy.setopt(CURLoption::CURLOPT_LOW_SPEED_LIMIT, timeout.low_speed);
        easy.setopt(CURLoption::CURLOPT_LOW_SPEED_TIME, timeout.transfer_timeout);
        easy.setopt(CURLoption::CURLOPT_CONNECTTIMEOUT, timeout.connect_timeout);
    }
    {
        auto& tcp = req.get_opt<req_opt::Tcp>();
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPALIVE, tcp.keepalive);
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPIDLE, tcp.keepidle);
        easy.setopt(CURLoption::CURLOPT_TCP_KEEPINTVL, tcp.keepintvl);
    }
    {
        auto& p = req.get_opt<req_opt::Proxy>();
        easy.setopt(CURLoption::CURLOPT_PROXYTYPE, p.type);
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
        rsp->set_share(p.share.clone());
    }
    easy.set_header(req.header());
}

} // namespace

ResponseBackend::Inner::Inner(ResponseBackend* res, const Request& req, http::Operation oper,
                              SessionBackend& ses)
    : m_q(res),
      m_req(req.clone()),
      m_operation(oper),
      m_finished(false),
      m_connect(Connection::make(ses.channel_rc(), ses.allocator())),
      m_allocator(ses.allocator()) {}

ResponseBackend::ResponseBackend(const Request& req, http::Operation oper,
                                 SessionBackend& ses) noexcept
    : m_inner(Arc<Inner>::make(this, req, oper, ses)) {
    auto* d    = m_inner.as_ptr().as_raw_ptr();
    auto& easy = connection().easy();
    switch (oper.tag()) {
    case http::Operation::Tag::Get: break;
    case http::Operation::Tag::Post:
        easy.setopt(CURLoption::CURLOPT_POST, 1);
        easy.setopt(CURLoption::CURLOPT_POSTFIELDS, nullptr);
        easy.setopt(CURLoption::CURLOPT_POSTFIELDSIZE_LARGE, 0);
        break;
    case http::Operation::Tag::Delete:
    case http::Operation::Tag::Head:
    default: break;
    }
    apply_easy_request(d, easy, req);
    {
        auto& p = req.get_opt<req_opt::Read>();
        if (p.callback) {
            connection().set_send_callback(p.callback);
        }
    }
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

auto ResponseBackend::allocator() const -> const std::pmr::polymorphic_allocator<char>& {
    return m_inner->m_allocator;
}

Arc<ResponseBackend> ResponseBackend::make_response(const Request& req, http::Operation oper,
                                                     SessionBackend& ses) {
    return Arc<ResponseBackend>::make(req, oper, ses);
}

const Request& ResponseBackend::request() const { return m_inner->m_req; }

bool ResponseBackend::pause_send(bool pause) {
    connection().send_action(pause ? Connection::Action::PauseSend : Connection::Action::UnPauseSend);
    return true;
}
bool ResponseBackend::pause_recv(bool pause) {
    connection().send_action(pause ? Connection::Action::PauseRecv : Connection::Action::UnPauseRecv);
    return true;
}

void ResponseBackend::add_send_buffer(rstd::bytes::Bytes buf) { m_inner->m_send_buffer = rstd::move(buf); }

void ResponseBackend::prepare_perform() {
    auto& easy = connection().easy();

    switch (m_inner->m_operation.tag()) {
    case http::Operation::Tag::Get: break;
    case http::Operation::Tag::Post: {
        auto& p = m_inner->m_req.get_opt<req_opt::Read>();
        if (p.callback) {
            easy.setopt(CURLoption::CURLOPT_POSTFIELDSIZE_LARGE, p.size ? p.size : -1);
        } else {
            auto& send_buffer = m_inner->m_send_buffer;
            easy.setopt(CURLoption::CURLOPT_POSTFIELDS, send_buffer.data());
            easy.setopt(CURLoption::CURLOPT_POSTFIELDSIZE_LARGE, send_buffer.size());
        }
        break;
    }
    case http::Operation::Tag::Delete:
    case http::Operation::Tag::Head:
    default: break;
    }

}

auto ResponseBackend::operation() const -> http::Operation { return m_inner->m_operation; }

bool ResponseBackend::is_finished() const {
    if (! m_inner) return true;
    return connection().is_finished();
}

auto ResponseBackend::header() const -> const http::Header& {
    return connection().header().headers();
}
auto ResponseBackend::head() const -> rstd::Option<rstd::ref<http::MessageHead>> {
    return Some(rstd::ref<http::MessageHead>::from_raw_parts(&connection().header()));
}
auto ResponseBackend::trailers() const -> rstd::Option<rstd::ref<http::Header>> {
    return connection().trailers();
}
auto ResponseBackend::code() const -> rstd::Option<i32> {
    auto status = connection().header().status_code();
    if (status.is_none()) return None();
    return Some(static_cast<i32>(*status));
}

auto ResponseBackend::connection() -> Connection& { return *(m_inner->m_connect); }
auto ResponseBackend::connection() const -> const Connection& { return *(m_inner->m_connect); }

void ResponseBackend::cancel() {
    if (! m_inner) return;
    connection().about_to_cancel();
}

auto ResponseBackend::bytes() -> coro<Result<rstd::bytes::Bytes>> {
    rstd::bytes::BytesMut out;
    auto                  chunk = rstd::bytes::BytesMut::with_capacity(ReadSize);

    for (;;) {
        chunk.clear();
        auto read = co_await connection().read_some(chunk);
        if (read.error.is_some()) {
            co_return Result<rstd::bytes::Bytes>(
                Err(rstd::move(read.error).unwrap_unchecked()));
        }
        if (read.eof) {
            break;
        }
        if (read.size == 0) {
            co_await rstd::async::yield_now();
            continue;
        }

        out.extend_from_slice(chunk.as_slice());
    }

    co_return Result<rstd::bytes::Bytes>(Ok(out.freeze()));
}

} // namespace ncrequest::client::curl
