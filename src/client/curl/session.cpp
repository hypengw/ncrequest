module;

#include <curl/curl.h>

#include <rstd/enum.hpp>
#include <rstd/macro.hpp>

module ncrequest;
import :client_curl_session;
import cppstd;

using namespace rstd::prelude;
using rstd::bytes::Bytes;
using rstd::path::Path;
using rstd::sync::Mutex;
using rstd::thread::JoinHandle;
using rstd::thread::spawn;
using rstd::time::Duration;
using std::pmr::memory_resource;
using std::pmr::polymorphic_allocator;

namespace ncrequest::client::curl
{

constexpr static auto POLL_TIMEOUT { Duration::from_millis(u64(1000)) };
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
    Private(memory_resource* mem_pool, CurlOptions options) noexcept;
    ~Private();

    void ensure_worker();
    void join_worker();
    void run();
    void handle_message(const SessionMessage&);

    void add_connect(const Arc<Connection>&);
    void remove_connect(const Arc<Connection>&);

private:
    Box<CurlMulti>       m_curl_multi;
    Vec<Arc<Connection>> m_connect_set;

    Arc<channel_type> m_channel;
    bool              m_stopped;

    memory_resource* m_memory;

    Mutex<Option<JoinHandle<void>>> m_thread;
};

SessionBackend::SessionBackend(memory_resource* mem_pool, CurlOptions options)
    : m_d(Box<Private>::make(mem_pool, options)) {}

void SessionBackend::start() { m_d->ensure_worker(); }

SessionBackend::~SessionBackend() {
    about_to_stop();
    m_d->join_worker();
    m_d->m_channel->set_wake_callback({});
}

auto SessionBackend::allocator() -> polymorphic_allocator<byte> { return { (m_d->m_memory) }; }

auto SessionBackend::perform(Arc<ResponseBackend>& rsp) -> coro<Result<empty>> {
    auto& con      = rsp->connection();
    auto  prepared = rsp->prepare_perform();
    if (prepared.is_err()) co_return Err(rstd::move(prepared).unwrap_err());

    auto msg = SessionMessage::ConnectAction(con.get_arc(), sm::Action::Add);
    if (! channel().try_send(rstd::move(msg))) co_return Err(Error::Canceled());

    auto header_error = co_await con.wait_header();
    if (header_error.is_some()) {
        co_return Result<empty>(Err(rstd::move(header_error).unwrap_unchecked()));
    }

    co_return Result<empty>(Ok(empty {}));
}

auto SessionBackend::start_request(PreparedRequest req) -> coro<Result<ResponseBackend>> {
    auto res       = ResponseBackend::make_response(rstd::move(req), *this);
    auto performed = co_await perform(res);
    if (performed.is_err()) co_return Err(rstd::move(performed).unwrap_err());
    co_return Ok(rstd::move(*res));
}

SessionBackend::Private::Private(memory_resource* mem_pool, CurlOptions options) noexcept
    : m_curl_multi(Box<CurlMulti>::make(options)),
      m_channel(Arc<channel_type>::make()),
      m_stopped(false),
      m_memory(mem_pool),
      m_thread(Option<JoinHandle<void>> {}) {
    m_channel->set_wake_callback([this] {
        m_curl_multi->wakeup();
    });
}

SessionBackend::Private::~Private() { join_worker(); }

void SessionBackend::Private::ensure_worker() {
    auto thread = m_thread.lock().unwrap();
    if (thread->is_some()) return;

    auto spawned = spawn([this] {
        run();
    });
    if (spawned.is_err()) rstd::panic { "failed to start curl session worker" };
    *thread = Some(rstd::move(spawned).unwrap());
}

void SessionBackend::Private::join_worker() {
    auto worker = Option<JoinHandle<void>> {};
    {
        auto thread = m_thread.lock().unwrap();
        worker      = thread->take();
    }
    if (worker.is_some()) {
        (void)rstd::move(*worker).join();
    }
}

void SessionBackend::load_cookie(ref<Path> path) { m_d->m_curl_multi->load_cookie(path); }
void SessionBackend::save_cookie(ref<Path> path) const { m_d->m_curl_multi->save_cookie(path); }

auto SessionBackend::cookies() -> Vec<String> { return m_d->m_curl_multi->cookies(); }

SessionBackend::channel_type& SessionBackend::channel() { return *(m_d->m_channel); }

auto SessionBackend::channel_rc() -> Arc<SessionBackend::channel_type> {
    return m_d->m_channel.clone();
}

void SessionBackend::about_to_stop() { channel().try_send(SessionMessage::Stop()); }

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
    for (usize i {}; i < m_connect_set.len(); ++i) {
        if (! Arc<Connection>::ptr_eq(m_connect_set[i], con)) continue;
        auto last = m_connect_set.len() - usize(1);
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
