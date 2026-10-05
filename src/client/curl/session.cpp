module;

#include <curl/curl.h>

#include <rstd/enum.hpp>
#include <rstd/macro.hpp>

module ncrequest;
import :client.curl.session;

using namespace rstd::prelude;
using rstd::bytes::Bytes;
using rstd::path::Path;
using rstd::sync::Arc;
using rstd::sync::Mutex;
using rstd::thread::JoinHandle;
using rstd::thread::spawn;
using rstd::time::Duration;

namespace ncrequest::client::curl
{

constexpr static auto POLL_TIMEOUT { Duration::from_millis(u64(1000)) };
namespace sm = ncrequest::client::curl::session_message;

class SessionBackend::Private {
    friend class SessionBackend;

public:
    Private(CurlOptions options) noexcept;
    ~Private();

    auto ensure_worker() -> Result<empty>;
    void join_worker();
    void run();
    void handle_message(SessionMessage&);

    auto add_connect(const Arc<Connection>&) -> Result<empty>;
    auto remove_connect(const Arc<Connection>&) -> bool;
    void stop(Option<CurlMultiError> error = None());

private:
    Box<CurlMulti>       m_curl_multi;
    Vec<Arc<Connection>> m_connect_set;

    Arc<channel_type> m_channel;
    bool              m_stopped;

    Mutex<Option<JoinHandle<void>>> m_thread;
};

SessionBackend::SessionBackend(CurlOptions options): m_d(Box<Private>::make(options)) {}

auto SessionBackend::start() -> Result<empty> { return m_d->ensure_worker(); }

SessionBackend::~SessionBackend() {
    about_to_stop();
    m_d->join_worker();
    m_d->m_channel->set_wake_callback({});
}

auto SessionBackend::perform(Arc<ResponseBackend>& rsp) -> coro<Result<empty>> {
    auto& con  = rsp->connection();
    auto  made = rstd::async::Completion<Result<empty>>::make();
    if (made.is_err()) co_return Err(Error::Io(rstd::move(made).unwrap_err()));
    auto pair  = rstd::move(made).unwrap();
    auto ready = rstd::move(pair.get<0>());
    auto msg   = SessionMessage::Start(con.get_arc(), rstd::move(pair.get<1>()));
    if (! channel().try_send(rstd::move(msg))) co_return Err(Error::Canceled());

    auto prepared = co_await rstd::move(ready);
    if (prepared.is_err()) co_return Err(Error::Canceled());
    auto result = rstd::move(prepared).unwrap();
    if (result.is_err()) co_return Err(rstd::move(result).unwrap_err());
    con.start_upload();

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

SessionBackend::Private::Private(CurlOptions options) noexcept
    : m_curl_multi(Box<CurlMulti>::make(options)),
      m_channel(Arc<channel_type>::make()),
      m_stopped(false),
      m_thread(Option<JoinHandle<void>> {}) {
    m_channel->set_wake_callback([this] {
        m_curl_multi->wakeup();
    });
}

SessionBackend::Private::~Private() { join_worker(); }

auto SessionBackend::Private::ensure_worker() -> Result<empty> {
    auto thread = m_thread.lock().unwrap();
    if (thread->is_some()) return Ok(empty {});
    auto initialized = m_curl_multi->initialization_result();
    if (initialized.is_err()) return Err(rstd::into<Error>(rstd::move(initialized).unwrap_err()));

    auto spawned = spawn([this] {
        run();
    });
    if (spawned.is_err()) return Err(Error::Io(rstd::move(spawned).unwrap_err()));
    *thread = Some(rstd::move(spawned).unwrap());
    return Ok(empty {});
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

SessionBackend::channel_type& SessionBackend::channel() { return *(m_d->m_channel); }

auto SessionBackend::channel_rc() -> Arc<SessionBackend::channel_type> {
    return m_d->m_channel.clone();
}

void SessionBackend::about_to_stop() { channel().try_send(SessionMessage::Stop()); }

auto SessionBackend::Private::add_connect(const Arc<Connection>& con) -> Result<empty> {
    auto prepared = con->prepare();
    if (prepared.is_err()) {
        con->cancel();
        con->release_easy();
        return prepared;
    }
    m_connect_set.push(con.clone());
    auto added = m_curl_multi->add_handle(con->easy());
    if (added.is_err()) {
        auto error = rstd::into<Error>(rstd::move(added).unwrap_err());
        if (remove_connect(con)) con->cancel();
        return Err(rstd::move(error));
    }
    con->transfreing();
    return Ok(empty {});
}

auto SessionBackend::Private::remove_connect(const Arc<Connection>& con) -> bool {
    for (usize i {}; i < m_connect_set.len(); ++i) {
        if (! Arc<Connection>::ptr_eq(m_connect_set[i], con)) continue;
        auto removed = m_curl_multi->remove_handle(con->easy());
        if (removed.is_err()) {
            stop(Some(rstd::move(removed).unwrap_err()));
            return false;
        }
        con->release_easy();
        auto last = m_connect_set.len() - usize(1);
        if (i != last) m_connect_set[i] = rstd::move(m_connect_set[last]);
        m_connect_set.pop_back();
        return true;
    }
    return false;
}

void SessionBackend::Private::stop(Option<CurlMultiError> error) {
    if (m_stopped) return;
    m_stopped = true;
    m_channel->try_send(SessionMessage::Stop());
    m_channel->set_wake_callback({});
    for (auto& con : m_connect_set) {
        auto removed = m_curl_multi->remove_handle(con->easy());
        if (removed.is_err() && error.is_none()) error = Some(rstd::move(removed).unwrap_err());
    }
    // If removal failed, destroy the multi before releasing any possibly attached easy.
    auto shutdown = m_curl_multi->shutdown();
    if (shutdown.is_err() && error.is_none()) error = Some(rstd::move(shutdown).unwrap_err());
    for (auto& con : m_connect_set) {
        if (error.is_some())
            con->fail(rstd::into<Error>(*error));
        else
            con->cancel();
        con->release_easy();
    }
    m_connect_set.clear();
    auto msg = SessionMessage {};
    while (m_channel->try_receive(msg)) {
        if (! msg.is_Start()) continue;
        auto& pending = msg.as_Start();
        pending.con->cancel();
        (void)pending.ready.complete(error.is_some() ? Result<empty>(Err(rstd::into<Error>(*error)))
                                                     : Result<empty>(Err(Error::Canceled())));
    }
}

void SessionBackend::Private::run() {
    while (! m_stopped) {
        while (m_connect_set.is_empty() && ! m_stopped) {
            auto msg = m_channel->receive();
            handle_message(msg);
        }
        auto msg = SessionMessage {};
        while (! m_stopped && m_channel->try_receive(msg)) handle_message(msg);
        if (m_stopped) break;

        int  running_connect { 0 };
        auto performed = m_curl_multi->perform(running_connect);
        if (performed.is_err()) {
            stop(Some(rstd::move(performed).unwrap_err()));
            break;
        }
        auto infos = m_curl_multi->query_info_msg();
        for (auto& m : infos) {
            if (m.msg != CURLMSG_DONE) continue;
            auto found = Option<Arc<Connection>> {};
            for (auto& active : m_connect_set) {
                if (active->easy().handle() == m.easy_handle) {
                    found = Some(active.clone());
                    break;
                }
            }
            if (found.is_none()) continue;
            auto con = rstd::move(found).unwrap();
            if (! remove_connect(con)) break;
            con->finish(m.result);
        }
        if (! m_stopped && ! m_connect_set.is_empty()) {
            auto polled = m_curl_multi->poll(POLL_TIMEOUT);
            if (polled.is_err()) stop(Some(rstd::move(polled).unwrap_err()));
        }
    }
}

void SessionBackend::Private::handle_message(SessionMessage& msg) {
    namespace sm = session_message;
    RSTD_MATCH(msg) {
        RSTD_CASE(Stop) { stop(); }
        RSTD_CASE(Start, con, ready) {
            if (ready.is_closed()) {
                con->cancel();
                return;
            }
            (void)ready.complete(add_connect(con));
        }
        RSTD_CASE(ConnectAction, con, action) {
            switch (action) {
                using enum sm::Action;
            case Cancel:
                if (remove_connect(con)) con->cancel();
                break;
            case PauseRecv:
            case UnPauseRecv:
            case PauseSend:
            case UnPauseSend: con->apply_pause(); break;
            }
        }
    }
}

} // namespace ncrequest::client::curl
