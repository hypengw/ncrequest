module;
#include "macro.hpp"
module ncrequest;
import :client_curl_response;
import :client_curl_session;
import ncrequest.coro;

namespace ncrequest::client::curl
{

namespace
{

void apply_easy_request(ResponseBackend::Inner* rsp, CurlEasy& easy, const Request& req) {
    easy.setopt(CURLoption::CURLOPT_URL, req.url().data());
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
            easy.setopt<CURLoption::CURLOPT_SHARE>(p.share->handle());
        }
        rsp->set_share(p.share.clone());
    }
    easy.set_header(req.header());
}

template<Attribute A, CURLINFO Info = to_curl_info(A)>
attr_value attr_from_easy(CurlEasy& easy) {
    auto res = easy.template get_info<attr_type<A>>(Info);
    return std::visit(helper::overloaded { [](auto a) {
                          return attr_value(a);
                      } },
                      res);
}

} // namespace

ResponseBackend::Inner::Inner(ResponseBackend* res, const Request& req, Operation oper,
                              Arc<SessionBackend> ses)
    : m_q(res),
      m_req(req.clone()),
      m_operation(oper),
      m_finished(false),
      m_connect(make_arc<Connection>(ses->channel_rc(), ses->allocator())),
      m_allocator(ses->allocator()) {}

ResponseBackend::ResponseBackend(const Request& req, Operation oper, Arc<SessionBackend> ses) noexcept
    : m_inner(make_arc<Inner>(this, req, oper, ses)) {
    auto  d    = m_inner.get();
    auto& easy = connection().easy();
    switch (oper) {
    case Operation::GetOperation: break;
    case Operation::PostOperation:
        easy.setopt(CURLoption::CURLOPT_POST, 1);
        easy.setopt(CURLoption::CURLOPT_POSTFIELDS, nullptr);
        easy.setopt(CURLoption::CURLOPT_POSTFIELDSIZE_LARGE, 0);
        break;
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

Arc<ResponseBackend> ResponseBackend::make_response(const Request& req, Operation oper,
                                                    Arc<SessionBackend> ses) {
    return std::make_shared<ResponseBackend>(req, oper, ses);
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

    switch (m_inner->m_operation) {
    case Operation::GetOperation: break;
    case Operation::PostOperation: {
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
    default: break;
    }

    connection().set_url(m_inner->m_req.url());
}

Operation ResponseBackend::operation() const { return m_inner->m_operation; }

bool ResponseBackend::is_finished() const {
    if (! m_inner) return true;
    return connection().is_finished();
}

attr_value ResponseBackend::attribute(Attribute A) const {
    auto& easy = connection().easy();
    switch (A) {
        using enum Attribute;
    case HttpCode: return attr_from_easy<HttpCode>(easy); break;
    default: break;
    }
    return {};
}

auto ResponseBackend::header() const -> const HttpHeader& { return connection().header(); }
auto ResponseBackend::code() const -> rstd::Option<i32> {
    auto& start = this->header().start;
    if (start) {
        if (auto status = std::get_if<HttpHeader::Status>(&*start)) {
            return Some((i32)(status->code));
        }
    }
    return None();
}

auto ResponseBackend::cookie_jar() const -> const CookieJar& { return connection().cookie_jar(); }

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

auto ResponseBackend::text() -> coro<Result<std::string>> {
    auto data_result = co_await bytes();
    if (data_result.is_err()) {
        auto err = rstd::move(data_result).unwrap_err();
        co_return Result<std::string>(Err(rstd::move(err)));
    }

    auto        data = rstd::move(data_result).unwrap();
    std::string out;
    out.assign(reinterpret_cast<const char*>(data.data()), data.size());
    co_return Result<std::string>(Ok(rstd::move(out)));
}

} // namespace ncrequest::client::curl
