module;

#include <curl/curl.h>

#include <rstd/enum.hpp>
#include <rstd/macro.hpp>

module ncrequest;
import :client_curl_session;
import cppstd;

namespace ncrequest::client::curl
{

constexpr static auto POLL_TIMEOUT { rstd::time::Duration::from_millis(1000) };
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
    Private(std::pmr::memory_resource* mem_pool, CurlOptions options) noexcept;
    ~Private();

    void ensure_worker();
    void join_worker();
    void run();
    void handle_message(const SessionMessage&);

    void add_connect(const Arc<Connection>&);
    void remove_connect(const Arc<Connection>&);

private:
    Box<CurlMulti>                    m_curl_multi;
    rstd::vec::Vec<Arc<Connection>>   m_connect_set;

    Arc<channel_type> m_channel;
    bool              m_stopped;

    rstd::Option<req_opt::Proxy> m_proxy;
    bool                         m_ignore_certificate;
    std::pmr::memory_resource*   m_memory;

    rstd::sync::Mutex<Option<rstd::thread::JoinHandle<void>>> m_thread;
};

SessionBackend::SessionBackend(std::pmr::memory_resource* mem_pool, CurlOptions options)
    : m_d(Box<Private>::make(mem_pool, options)) {}

void SessionBackend::start() {
    m_d->ensure_worker();
}

SessionBackend::~SessionBackend() {
    about_to_stop();
    m_d->join_worker();
    m_d->m_channel->set_wake_callback({});
}

auto SessionBackend::allocator() -> std::pmr::polymorphic_allocator<byte> {
    return { (m_d->m_memory) };
}

auto SessionBackend::prepare_req(const Request& req) const -> Request {
    Request o { req.clone() };
    if (m_d->m_proxy) o.set_opt(m_d->m_proxy.clone().unwrap());
    if (m_d->m_ignore_certificate) o.get_opt<req_opt::SSL>().verify_certificate = false;
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

auto SessionBackend::start_request(const Request& req, http::Operation operation,
                                   rstd::Option<rstd::bytes::Bytes> body)
    -> coro<Result<ResponseBackend>> {
    Arc<ResponseBackend> res =
        ResponseBackend::make_response(prepare_req(req), operation, *this);
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
        ResponseBackend::make_response(prepare_req(req), http::Operation::Get(), *this);

    auto performed = co_await perform(res);
    if (performed.is_ok()) {
        co_return Result<Arc<ResponseBackend>>(Ok(rstd::move(res)));
    }
    co_return Result<Arc<ResponseBackend>>(Err(rstd::move(performed).unwrap_err()));
}

auto SessionBackend::post(const Request& req) -> coro<Result<Arc<ResponseBackend>>> {
    Arc<ResponseBackend> res =
        ResponseBackend::make_response(prepare_req(req), http::Operation::Post(), *this);
    auto performed = co_await perform(res);
    if (performed.is_ok()) {
        co_return Result<Arc<ResponseBackend>>(Ok(rstd::move(res)));
    }
    co_return Result<Arc<ResponseBackend>>(Err(rstd::move(performed).unwrap_err()));
}

auto SessionBackend::post(const Request& req, rstd::bytes::Bytes body)
    -> coro<Result<Arc<ResponseBackend>>> {
    Arc<ResponseBackend> res =
        ResponseBackend::make_response(prepare_req(req), http::Operation::Post(), *this);
    res->add_send_buffer(rstd::move(body));

    auto performed = co_await perform(res);
    if (performed.is_ok()) {
        co_return Result<Arc<ResponseBackend>>(Ok(rstd::move(res)));
    }
    co_return Result<Arc<ResponseBackend>>(Err(rstd::move(performed).unwrap_err()));
}

SessionBackend::Private::Private(std::pmr::memory_resource* mem_pool,
                                 CurlOptions options) noexcept
    : m_curl_multi(Box<CurlMulti>::make(options)),
      m_channel(Arc<channel_type>::make()),
      m_stopped(false),
      m_proxy(),
      m_ignore_certificate(false),
      m_memory(mem_pool),
      m_thread(Option<rstd::thread::JoinHandle<void>> {}) {
    m_channel->set_wake_callback([this] {
        m_curl_multi->wakeup();
    });
}

SessionBackend::Private::~Private() {
    join_worker();
}

void SessionBackend::Private::ensure_worker() {
    auto thread = m_thread.lock().unwrap();
    if (thread->is_some()) return;

    auto spawned = rstd::thread::spawn([this] {
        run();
    });
    if (spawned.is_err()) rstd::panic { "failed to start curl session worker" };
    *thread = Some(rstd::move(spawned).unwrap());
}

void SessionBackend::Private::join_worker() {
    auto worker = Option<rstd::thread::JoinHandle<void>> {};
    {
        auto thread = m_thread.lock().unwrap();
        worker      = thread->take();
    }
    if (worker.is_some()) {
        (void)rstd::move(*worker).join();
    }
}

void SessionBackend::load_cookie(ref<rstd::path::Path> path) {
    m_d->m_curl_multi->load_cookie(path);
}
void SessionBackend::save_cookie(ref<rstd::path::Path> path) const {
    m_d->m_curl_multi->save_cookie(path);
}

auto SessionBackend::cookies() -> rstd::vec::Vec<rstd::string::String> {
    return m_d->m_curl_multi->cookies();
}
void SessionBackend::set_proxy(const req_opt::Proxy& p) {
    m_d->m_proxy = Some(p.clone());
}
void SessionBackend::set_verify_certificate(bool v) {
    m_d->m_ignore_certificate = ! v;
}

SessionBackend::channel_type& SessionBackend::channel() {
    return *(m_d->m_channel);
}

auto SessionBackend::channel_rc() -> Arc<SessionBackend::channel_type> {
    return m_d->m_channel.clone();
}

void SessionBackend::about_to_stop() {
    channel().try_send(SessionMessage::Stop());
}

void SessionBackend::Private::add_connect(const Arc<Connection>& con) {
    auto added = m_curl_multi->add_handle(con->easy());
    if (added.is_err()) {
        con->finish(CURLcode::CURLE_FAILED_INIT);
        return;
    }
    con->transfreing();
    m_connect_set.push(con.clone());
}
void SessionBackend::Private::remove_connect(const Arc<Connection>& con) {
    (void)m_curl_multi->remove_handle(con->easy());
    for (usize i = 0; i < m_connect_set.len(); ++i) {
        if (! Arc<Connection>::ptr_eq(m_connect_set[i], con)) continue;
        auto last = m_connect_set.len() - 1;
        if (i != last) m_connect_set[i] = rstd::move(m_connect_set[last]);
        m_connect_set.pop_back();
        return;
    }
}

void SessionBackend::Private::run() {
    do {
        while (m_connect_set.is_empty() && ! m_stopped) {
            auto msg = m_channel->receive();
            handle_message(msg);
        }

        auto msg = SessionMessage {};
        while (m_channel->try_receive(msg)) {
            handle_message(msg);
        }

        int running_connect { 0 };
        (void)m_curl_multi->perform(running_connect);

        auto infos = m_curl_multi->query_info_msg();
        for (auto& m : infos) {
            if (m.msg != CURLMSG_DONE) continue;
            auto con = get_curl_private<Connection*>(m.easy_handle)->get_arc();
            con->finish(m.result);
            remove_connect(con);
            running_connect--;
        }

        if (running_connect > 0) {
            (void)m_curl_multi->poll(POLL_TIMEOUT);
        }
    } while (! m_stopped);
}

void SessionBackend::Private::handle_message(const SessionMessage& msg) {
    namespace sm = session_message;
    RSTD_MATCH(msg) {
        RSTD_CASE(Stop) {
            m_stopped = true;
            while (! m_connect_set.is_empty()) {
                auto con = rstd::move(m_connect_set.pop()).unwrap_unchecked();
                con->cancel();
                (void)m_curl_multi->remove_handle(con->easy());
            }
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
