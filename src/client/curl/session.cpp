module;

#include <curl/curl.h>
#include <mutex>
#include <thread>

#include "log.hpp"
#include "macro.hpp"
#include <rstd/enum.hpp>
#include <rstd/macro.hpp>

module ncrequest;
import :client_curl_session;
import cppstd;

namespace ncrequest::client::curl
{

constexpr static auto POLL_TIMEOUT { std::chrono::milliseconds(1000) };
namespace sm = ncrequest::client::curl::session_message;

namespace
{

template<typename T>
T get_curl_private(CURL* c) {
    T        easy { nullptr };
    CURLcode rc = curl_easy_getinfo(c, CURLINFO_PRIVATE, &easy);
    rstd_assert(! rc);
    return easy;
}

} // namespace

class SessionBackend::Private {
    friend class SessionBackend;

public:
    Private(SessionBackend&, std::pmr::memory_resource* mem_pool, CurlOptions options) noexcept;
    ~Private();

    void start();
    void run();
    void handle_message(const SessionMessage&);

    void add_connect(const Arc<Connection>&);
    void remove_connect(const Arc<Connection>&);

private:
    Box<CurlMulti>            m_curl_multi;
    std::set<Arc<Connection>> m_connect_set;

    Arc<channel_type> m_channel;
    bool              m_stopped;

    rstd::Option<req_opt::Proxy> m_proxy;
    bool                         m_ignore_certificate;
    std::pmr::memory_resource*   m_memory;

    std::thread m_thread;
    std::mutex  m_start_mutex;
};

SessionBackend::SessionBackend(std::pmr::memory_resource* mem_pool, CurlOptions options)
    : m_d(std::make_unique<Private>(*this, mem_pool, options)) {}

void SessionBackend::start() {
    C_D(SessionBackend);
    d->start();
}

SessionBackend::~SessionBackend() {
    C_D(SessionBackend);
    about_to_stop();
    if (d->m_thread.joinable()) {
        d->m_thread.join();
    }
    d->m_channel->set_wake_callback({});
}

auto SessionBackend::allocator() -> std::pmr::polymorphic_allocator<byte> {
    C_D(SessionBackend);
    return { (d->m_memory) };
}

auto SessionBackend::prepare_req(const Request& req) const -> Request {
    C_D(const SessionBackend);
    Request o { req.clone() };
    if (d->m_proxy) o.set_opt(d->m_proxy.clone().unwrap());
    if (d->m_ignore_certificate) o.get_opt<req_opt::SSL>().verify_certificate = false;
    return o;
}

auto SessionBackend::perform(Arc<ResponseBackend>& rsp) -> coro<Result<rstd::empty>> {
    auto& con = rsp->connection();
    rsp->prepare_perform();

    auto msg = SessionMessage::ConnectAction(con.get_arc(), sm::Action::Add);
    channel().try_send(rstd::move(msg));

    auto header_error = co_await con.wait_header();
    if (header_error.is_some()) {
        co_return Result<rstd::empty>(Err(rstd::move(header_error).unwrap_unchecked()));
    }

    co_return Result<rstd::empty>(Ok(rstd::empty {}));
}

auto SessionBackend::start_request(const Request& req, Operation operation,
                                   rstd::Option<rstd::bytes::Bytes> body)
    -> coro<Result<ResponseBackend>> {
    Arc<ResponseBackend> res =
        ResponseBackend::make_response(prepare_req(req), operation, shared_from_this());
    if (body.is_some()) {
        res->add_send_buffer(rstd::move(body).unwrap_unchecked());
    }

    auto performed = co_await perform(res);
    if (performed.is_err()) {
        co_return Result<ResponseBackend>(Err(rstd::move(performed).unwrap_err()));
    }

    co_return Result<ResponseBackend>(Ok(rstd::move(*res)));
}

auto SessionBackend::get(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    auto res =
        ResponseBackend::make_response(prepare_req(req), Operation::GetOperation, shared_from_this());

    auto performed = co_await perform(res);
    if (performed.is_ok()) {
        co_return Result<Arc<ResponseBackend>>(Ok(rstd::move(res)));
    }
    co_return Result<Arc<ResponseBackend>>(Err(rstd::move(performed).unwrap_err()));
}

auto SessionBackend::post(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    Arc<ResponseBackend> res =
        ResponseBackend::make_response(prepare_req(req), Operation::PostOperation, shared_from_this());
    auto performed = co_await perform(res);
    if (performed.is_ok()) {
        co_return Result<Arc<ResponseBackend>>(Ok(rstd::move(res)));
    }
    co_return Result<Arc<ResponseBackend>>(Err(rstd::move(performed).unwrap_err()));
}

auto SessionBackend::post(const Request& req, rstd::bytes::Bytes body)
    -> coro<Result<Arc<ResponseBackend>>> {
    Arc<ResponseBackend> res =
        ResponseBackend::make_response(prepare_req(req), Operation::PostOperation, shared_from_this());
    res->add_send_buffer(rstd::move(body));

    auto performed = co_await perform(res);
    if (performed.is_ok()) {
        co_return Result<Arc<ResponseBackend>>(Ok(rstd::move(res)));
    }
    co_return Result<Arc<ResponseBackend>>(Err(rstd::move(performed).unwrap_err()));
}

SessionBackend::Private::Private(SessionBackend&, std::pmr::memory_resource* mem_pool,
                                 CurlOptions options) noexcept
    : m_curl_multi(std::make_unique<CurlMulti>(options)),
      m_channel(std::make_shared<channel_type>()),
      m_stopped(false),
      m_proxy(),
      m_ignore_certificate(false),
      m_memory(mem_pool) {
    m_channel->set_wake_callback([this] {
        m_curl_multi->wakeup();
    });
}

SessionBackend::Private::~Private() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SessionBackend::Private::start() {
    auto lock = std::lock_guard { m_start_mutex };
    if (m_thread.joinable()) return;

    m_thread = std::thread([this] {
        run();
    });
}

void SessionBackend::load_cookie(std::filesystem::path p) {
    C_D(SessionBackend);
    d->m_curl_multi->load_cookie(p);
}
void SessionBackend::save_cookie(std::filesystem::path p) const {
    C_D(const SessionBackend);
    d->m_curl_multi->save_cookie(p);
}

auto SessionBackend::cookies() -> std::vector<std::string> {
    C_D(const SessionBackend);
    return d->m_curl_multi->cookies();
}
void SessionBackend::set_proxy(const req_opt::Proxy& p) {
    C_D(SessionBackend);
    d->m_proxy = Some(p.clone());
}
void SessionBackend::set_verify_certificate(bool v) {
    C_D(SessionBackend);
    d->m_ignore_certificate = ! v;
}

SessionBackend::channel_type& SessionBackend::channel() {
    C_D(SessionBackend);
    return *(d->m_channel);
}

auto SessionBackend::channel_rc() -> Arc<SessionBackend::channel_type> {
    C_D(SessionBackend);
    return d->m_channel;
}

void SessionBackend::about_to_stop() {
    channel().try_send(SessionMessage::Stop());
}

void SessionBackend::Private::add_connect(const Arc<Connection>& con) {
    auto ec = m_curl_multi->add_handle(con->easy());
    if (ec) {
        ERROR_LOG("{}", ec.message());
        con->finish(CURLcode::CURLE_FAILED_INIT);
        return;
    }
    DEBUG_LOG("add {}", con->url());
    con->transfreing();
    m_connect_set.insert(con);
}
void SessionBackend::Private::remove_connect(const Arc<Connection>& con) {
    DEBUG_LOG("end {}", con->url());
    auto ec = m_curl_multi->remove_handle(con->easy());
    m_connect_set.erase(con);
    if (ec) {
        ERROR_LOG("{}", ec.message());
    }
}

void SessionBackend::Private::run() {
    do {
        while (m_connect_set.empty() && ! m_stopped) {
            auto msg = m_channel->receive();
            handle_message(msg);
        }

        auto msg = SessionMessage {};
        while (m_channel->try_receive(msg)) {
            handle_message(msg);
        }

        int running_connect { 0 };
        if (auto re = m_curl_multi->perform(running_connect); re) {
            ERROR_LOG("{}", re.message());
        };

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

void SessionBackend::Private::handle_message(const SessionMessage& msg) {
    namespace sm = session_message;
    RSTD_MATCH(msg) {
        RSTD_CASE(Stop) {
            m_stopped = true;
            while (m_connect_set.size()) {
                auto& con = *m_connect_set.begin();
                con->cancel();
                remove_connect(con);
            }
            m_connect_set.clear();
        }
        RSTD_CASE(ConnectAction, con, action) {
            switch (action) {
                using enum sm::Action;
            case Add: add_connect(con); break;
            case Cancel:
                con->cancel();
                remove_connect(con);
                break;
            case PauseRecv: con->easy().pause(CURLPAUSE_RECV); break;
            case UnPauseRecv: con->easy().pause(CURLPAUSE_RECV_CONT); break;
            case PauseSend: con->easy().pause(CURLPAUSE_SEND); break;
            case UnPauseSend: con->easy().pause(CURLPAUSE_SEND_CONT); break;
            }
        }
    }
}

} // namespace ncrequest::client::curl
