module;

#include <format>
#include <cstdio>
#include <cassert>
#include <variant>
#include <coroutine>

#include <curl/curl.h>
#include <asio/buffer.hpp>
#include <asio/any_completion_handler.hpp>
#include <asio/associated_executor.hpp>
#include <asio/dispatch.hpp>
#include <asio/streambuf.hpp>
#include <asio/bind_executor.hpp>
#include <asio/as_tuple.hpp>
#include <asio/read.hpp>

#include "macro.hpp"

module ncrequest;
import :response;
import :session;
import ncrequest.coro;

using namespace ncrequest;

namespace
{

void apply_easy_request(Response::Inner* rsp, CurlEasy& easy, const Request& req) {
    easy.setopt(CURLOPT_URL, req.url().data());
    {
        auto& timeout = req.get_opt<req_opt::Timeout>();

        easy.setopt(CURLOPT_LOW_SPEED_LIMIT, timeout.low_speed);
        easy.setopt(CURLOPT_LOW_SPEED_TIME, timeout.transfer_timeout);
        easy.setopt(CURLOPT_CONNECTTIMEOUT, timeout.connect_timeout);
    }
    {
        auto& tcp = req.get_opt<req_opt::Tcp>();
        easy.setopt(CURLOPT_TCP_KEEPALIVE, tcp.keepalive);
        easy.setopt(CURLOPT_TCP_KEEPIDLE, tcp.keepidle);
        easy.setopt(CURLOPT_TCP_KEEPINTVL, tcp.keepintvl);
    }
    {
        auto& p = req.get_opt<req_opt::Proxy>();
        easy.setopt(CURLOPT_PROXYTYPE, p.type);
        easy.setopt(CURLOPT_PROXY, p.content.empty() ? NULL : p.content.c_str());
    }
    {
        auto& p = req.get_opt<req_opt::SSL>();
        easy.setopt(CURLOPT_SSL_VERIFYPEER, (long)p.verify_certificate);
        easy.setopt(CURLOPT_PROXY_SSL_VERIFYPEER, (long)p.verify_certificate);
    }
    {
        auto& p = req.get_opt<req_opt::Share>();
        if (p.share) {
            easy.setopt<CURLOPT_SHARE>(p.share->handle());
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

Response::Inner::Inner(Response* res, const Request& req, Operation oper, Arc<Session> ses)
    : m_q(res),
      m_req(req.clone()),
      m_operation(oper),
      m_connect(make_arc<Connection>(ses->get_executor(), ses->channel_rc(), ses->allocator())),
      m_allocator(ses->allocator()) {}

Response::Response(const Request& req, Operation oper, Arc<Session> ses) noexcept
    : m_inner(make_arc<Inner>(this, req, oper, ses)) {
    auto  d    = m_inner.get();
    auto& easy = connection().easy();
    switch (oper) {
    case Operation::GetOperation: break;
    case Operation::PostOperation:
        easy.setopt(CURLOPT_POST, 1);
        easy.setopt(CURLOPT_POSTFIELDS, NULL);
        easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE, 0);
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
Response::Response(Response&&) noexcept            = default;
Response& Response::operator=(Response&&) noexcept = default;
Response::~Response() noexcept { cancel(); }

auto Response::allocator() const -> const std::pmr::polymorphic_allocator<char>& {
    return m_inner->m_allocator;
}

Arc<Response> Response::make_response(const Request& req, Operation oper, Arc<Session> ses) {
    return std::make_shared<Response>(req, oper, ses);
}

const Request& Response::request() const { return m_inner->m_req; }

bool Response::pause_send(bool) {
    //    C_D(Response);
    //    return m_inner->m_easy->pause(val ? CURLPAUSE_SEND : CURLPAUSE_SEND_CONT) == CURLE_OK;
    return true;
}
bool Response::pause_recv(bool) {
    //    C_D(Response);
    //    return m_inner->m_easy->pause(val ? CURLPAUSE_RECV : CURLPAUSE_RECV_CONT) == CURLE_OK;
    return true;
}

void Response::add_send_buffer(asio::const_buffer buf) {
    auto& send_buf = m_inner->m_send_buffer;
    send_buf.commit(asio::buffer_copy(send_buf.prepare(buf.size()), buf));
}

void Response::async_read_some_impl(
    asio::mutable_buffer                                        buffer,
    asio::any_completion_handler<void(asio::error_code, usize)> handler) {
    connection().async_read_some(buffer, std::move(handler));
}

void Response::async_write_some_impl(
    asio::const_buffer                                          buffer,
    asio::any_completion_handler<void(asio::error_code, usize)> handler) {
    connection().async_write_some(buffer, std::move(handler));
}

void Response::prepare_perform() {
    auto& easy = connection().easy();

    switch (m_inner->m_operation) {
    case Operation::GetOperation: break;
    case Operation::PostOperation: {
        auto& p = m_inner->m_req.get_opt<req_opt::Read>();
        if (p.callback) {
            easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE, p.size ? p.size : -1);
        } else {
            auto& send_buffer = m_inner->m_send_buffer;
            easy.setopt(CURLOPT_POSTFIELDS, send_buffer.data());
            easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE, send_buffer.size());
        }
        break;
    }
    default: break;
    }

    connection().set_url(m_inner->m_req.url());
}

Operation Response::operation() const { return m_inner->m_operation; }

Response::executor_type& Response::get_executor() { return connection().get_executor(); }

bool Response::is_finished() const { return false; }

attr_value Response::attribute(Attribute A) const {
    auto& easy = connection().easy();
    switch (A) {
        using enum Attribute;
    case HttpCode: return attr_from_easy<HttpCode>(easy); break;
    default: break;
    }
    return {};
}

auto Response::header() const -> const HttpHeader& { return connection().header(); }
auto Response::code() const -> rstd::Option<i32> {
    auto& start = this->header().start;
    if (start) {
        if (auto status = std::get_if<HttpHeader::Status>(&*start)) {
            return Some((i32)(status->code));
        }
    }
    return None();
}
auto Response::connection() -> Connection& { return *(m_inner->m_connect); }
auto Response::connection() const -> const Connection& { return *(m_inner->m_connect); }

void Response::cancel() { connection().about_to_cancel(); }

auto Response::text() -> coro<Result<std::string>> {
    asio::basic_streambuf<allocator_type> buf(std::numeric_limits<usize>::max(), allocator());
    buf.prepare(ReadSize);

    auto [ec, size] =
        co_await asio::async_read(*this,
                                  buf,
                                  asio::transfer_all(),
                                  asio::as_tuple(asio::bind_executor(get_executor(), use_coro)));
    if (ec != asio::stream_errc::eof) {
        asio::detail::throw_error(ec);
    }
    std::string out;
    out.resize(buf.in_avail());
    buf.sgetn((char*)(out.data()), out.size());
    co_return Ok(std::move(out));
}
auto Response::bytes() -> coro<Result<std::vector<byte>>> {
    std::vector<byte>                     out;
    asio::basic_streambuf<allocator_type> buf(std::numeric_limits<usize>::max(), allocator());
    buf.prepare(ReadSize);

    auto [ec, size] =
        co_await asio::async_read(*this,
                                  buf,
                                  asio::transfer_all(),
                                  asio::as_tuple(asio::bind_executor(get_executor(), use_coro)));
    if (ec != asio::stream_errc::eof) {
        asio::detail::throw_error(ec);
    }
    out.resize(buf.in_avail());
    buf.sgetn((char*)(out.data()), out.size());
    co_return Ok(std::move(out));
}