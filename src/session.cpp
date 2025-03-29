module;
#include <asio/deferred.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/strand.hpp>
#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/experimental/concurrent_channel.hpp>

#include <iostream>
#include <chrono>
#include <set>
#include <filesystem>
#include <memory_resource>
#include <curl/curl.h>

#include "log.hpp"
#include "macro.hpp"

module ncrequest;
import :session;

using namespace ncrequest;

constexpr static auto POLL_TIMEOUT { std::chrono::milliseconds(1000) };
namespace sm = session_message;

namespace
{

template<typename T>
T get_curl_private(CURL* c) {
    T        easy { nullptr };
    CURLcode rc = curl_easy_getinfo(c, CURLINFO_PRIVATE, &easy);
    assert(! rc);
    return easy;
}

} // namespace

class Session::Private {
    friend class Session;

public:
    using channel_poll_type =
        asio::experimental::concurrent_channel<executor_type,
                                               void(asio::error_code, SessionMessage)>;

    Private(Session&, executor_type& ex, std::pmr::memory_resource* mem_pool) noexcept;

    coro<void> run();
    void       handle_message(const SessionMessage&);

    void add_connect(const Arc<Connection>&);
    void remove_connect(const Arc<Connection>&);

private:
    Session&                    m_p;
    Box<CurlMulti>              m_curl_multi;
    executor_type               m_ex;
    asio::strand<executor_type> m_strand;
    std::set<Arc<Connection>>   m_connect_set;

    asio::thread_pool      m_poll_thread;
    Arc<channel_poll_type> m_channel;
    Arc<channel_type>      m_channel_with_notify;
    bool                   m_stopped;

    rstd::Option<req_opt::Proxy>         m_proxy;
    bool                                 m_ignore_certificate;
    std::pmr::synchronized_pool_resource m_memory;
};

Session::Session(executor_type ex, std::pmr::memory_resource* mem_pool)
    : m_d(std::make_unique<Private>(*this, ex, mem_pool)) {}

auto Session::make(executor_type ex, std::pmr::memory_resource* memory) -> Arc<Session> {
    struct Session_ : Session {
        Session_(executor_type ex, std::pmr::memory_resource* memory): Session(ex, memory) {}
    };
    Arc<Session> session = make_arc<Session_>(ex, memory);
    auto         d       = session->m_d.get();

    asio::co_spawn(d->m_poll_thread.get_executor(), d->run(), asio::detached);
    asio::co_spawn(
        d->m_channel_with_notify->get_executor(),
        [self_weak = session->weak_from_this(),
         channel   = d->m_channel_with_notify]() -> coro<void> {
            for (;;) {
                auto [ec, msg] = co_await channel->async_receive(asio::as_tuple(use_coro));
                bool stopped   = std::get_if<sm::Stop>(&msg);
                if (auto self = self_weak.lock(); self && ! stopped) {
                    auto d       = self->m_d.get();
                    bool send_ok = d->m_channel->try_send(ec, msg);
                    // notify
                    d->m_curl_multi->wakeup();
                    // if channel full, wait
                    if (! send_ok) {
                        co_await d->m_channel->async_send(ec, msg, use_coro);
                    }
                } else {
                    break;
                }
            }
            co_return;
        },
        asio::detached);
    return session;
}

Session::~Session() {
    C_D(Session);
    about_to_stop();

    d->m_poll_thread.join();
}

auto Session::get_executor() -> Session::executor_type& {
    C_D(Session);
    return d->m_ex;
}
auto Session::get_strand() -> asio::strand<Session::executor_type>& {
    C_D(Session);
    return d->m_strand;
}

auto Session::allocator() -> std::pmr::polymorphic_allocator<byte> {
    C_D(Session);
    return { &(d->m_memory) };
}

auto Session::prepare_req(const Request& req) const -> Request {
    C_D(const Session);
    Request o{ req.clone() };
    if (d->m_proxy) o.set_opt(d->m_proxy.clone().unwrap());
    if (d->m_ignore_certificate) o.get_opt<req_opt::SSL>().verify_certificate = false;
    return std::move(o);
}

auto Session::perform(Arc<Response>& rsp) -> coro<bool> {
    C_D(Session);
    auto& con = rsp->connection();
    rsp->prepare_perform();

    sm::ConnectAction msg {
        .con    = con.get_arc(),
        .action = sm::ConnectAction::Action::Add,
    };

    co_await channel().async_send(asio::error_code {}, msg, use_coro);

    co_await con.async_wait_header(use_coro);
    co_return true;
}

auto Session::get(const Request& req) -> coro<rstd::Option<Arc<Response>>> {
    C_D(Session);
    auto res =
        Response::make_response(prepare_req(req), Operation::GetOperation, shared_from_this());

    if (co_await perform(res)) co_return Some(std::move(res));
    co_return None();
}

auto Session::post(const Request& req) -> coro<rstd::Option<Arc<Response>>> {
    C_D(Session);
    Arc<Response> res =
        Response::make_response(prepare_req(req), Operation::PostOperation, shared_from_this());
    if (co_await perform(res)) co_return Some(std::move(res));
    co_return None();
}

auto Session::post(const Request& req, asio::const_buffer buf)
    -> coro<rstd::Option<Arc<Response>>> {
    C_D(Session);
    Arc<Response> res =
        Response::make_response(prepare_req(req), Operation::PostOperation, shared_from_this());
    res->add_send_buffer(buf);

    if (co_await perform(res)) co_return Some(std::move(res));
    co_return None();
}

Session::Private::Private(Session& p, executor_type& ex,
                          std::pmr::memory_resource* mem_pool) noexcept
    : m_p(p),
      m_curl_multi(std::make_unique<CurlMulti>()),
      m_ex(ex),
      m_strand(ex),
      m_poll_thread(1),
      m_channel(std::make_shared<channel_poll_type>(m_poll_thread.get_executor(), 1024)),
      m_channel_with_notify(std::make_shared<channel_type>(asio::make_strand(ex), 1024)),
      m_stopped(false),
      m_proxy(),
      m_ignore_certificate(false),
      // 1 MB
      m_memory(std::pmr::pool_options { .max_blocks_per_chunk        = 2,
                                        .largest_required_pool_block = 1024 * 1024 },
               mem_pool) {};

void Session::load_cookie(std::filesystem::path p) {
    C_D(Session);
    d->m_curl_multi->load_cookie(p);
}
void Session::save_cookie(std::filesystem::path p) const {
    C_D(const Session);
    d->m_curl_multi->save_cookie(p);
}

auto Session::cookies() -> std::vector<std::string> {
    C_D(const Session);
    return d->m_curl_multi->cookies();
}
void Session::set_proxy(const req_opt::Proxy& p) {
    C_D(Session);
    d->m_proxy = Some(p.clone());
}
void Session::set_verify_certificate(bool v) {
    C_D(Session);
    d->m_ignore_certificate = ! v;
}

Session::channel_type& Session::channel() {
    C_D(Session);
    return *(d->m_channel_with_notify);
}

auto Session::channel_rc() -> Arc<Session::channel_type> {
    C_D(Session);
    return d->m_channel_with_notify;
}

void Session::about_to_stop() {
    C_D(Session);
    channel().try_send(asio::error_code {}, sm::Stop {});
    d->m_channel->try_send(asio::error_code {}, sm::Stop {});
    // notify
    d->m_curl_multi->wakeup();
}

void Session::Private::add_connect(const Arc<Connection>& con) {
    auto ec = m_curl_multi->add_handle(con->easy());
    if (ec) {
        ERROR_LOG("{}", ec.message());
        return;
    }
    DEBUG_LOG("add {}", con->url());
    con->transfreing();
    m_connect_set.insert(con);
}
void Session::Private::remove_connect(const Arc<Connection>& con) {
    DEBUG_LOG("end {}", con->url());
    auto ec = m_curl_multi->remove_handle(con->easy());
    m_connect_set.erase(con);
    if (ec) {
        ERROR_LOG("{}", ec.message());
    }
}

auto Session::Private::run() -> coro<void> {
    do {
        while (m_connect_set.empty() && ! m_stopped) {
            auto msg = co_await m_channel->async_receive(use_coro);
            handle_message(msg);
        }
        while (m_channel->try_receive([this](auto, const auto& m) {
            handle_message(m);
        })) {
        }

        int running_connect { 0 };
        if (auto re = m_curl_multi->perform(running_connect); re) {
            ERROR_LOG("{}", re.message());
        };

        // check done before poll, to make sure multi has active sockets to poll
        auto infos = m_curl_multi->query_info_msg();
        for (auto& m : infos) {
            if (m.msg != CURLMSG_DONE) continue;
            auto con = get_curl_private<Connection*>(m.easy_handle)->get_arc();
            con->finish(m.result);
            remove_connect(con);
            running_connect--;
        }

        if (running_connect > 0) {
            if (auto re = m_curl_multi->poll(POLL_TIMEOUT); re) {
                ERROR_LOG("{}", re.message());
            }
        }

        if (m_connect_set.empty()) {
            DEBUG_LOG("all connection finished");
        }
    } while (! m_stopped);
    DEBUG_LOG("session stopped");
}

void Session::Private::handle_message(const SessionMessage& msg) {
    namespace sm = session_message;
    std::visit(helper::overloaded { [this](sm::Stop) {
                                       m_stopped = true;
                                       while (m_connect_set.size()) {
                                           auto& con = *m_connect_set.begin();
                                           con->cancel();
                                           remove_connect(con);
                                       }
                                       m_connect_set.clear();
                                   },
                                    [this](const sm::ConnectAction& con_act) {
                                        switch (con_act.action) {
                                            using enum sm::ConnectAction::Action;
                                        case Add: add_connect(con_act.con); break;
                                        case Cancel:
                                            con_act.con->cancel();
                                            remove_connect(con_act.con);
                                            break;
                                        case PauseRecv:
                                            con_act.con->easy().pause(CURLPAUSE_RECV);
                                            break;
                                        case UnPauseRecv:
                                            con_act.con->easy().pause(CURLPAUSE_RECV_CONT);
                                            break;
                                        case PauseSend:
                                            con_act.con->easy().pause(CURLPAUSE_SEND);
                                            break;
                                        case UnPauseSend:
                                            con_act.con->easy().pause(CURLPAUSE_SEND_CONT);
                                            break;
                                        default: break;
                                        }
                                    } },
               msg);
}
