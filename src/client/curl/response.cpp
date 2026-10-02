module;
module ncrequest;
import :client_curl_response;
import :client_curl_session;
import ncrequest.coro;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::async::yield_now;
using rstd::bytes::Bytes;
using rstd::bytes::BytesMut;
using rstd::sync::Arc;

namespace ncrequest::client::curl
{

ResponseBackend::Inner::Inner(ResponseBackend* res, PreparedRequest req, SessionBackend& ses)
    : m_q(res), m_finished(false), m_connect(Connection::make(rstd::move(req), ses.channel_rc())) {}

ResponseBackend::ResponseBackend(PreparedRequest req, SessionBackend& ses) noexcept
    : m_inner(Arc<Inner>::make(this, rstd::move(req), ses)) {
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
    connection().request_pause(false, pause);
    return true;
}
bool ResponseBackend::pause_recv(bool pause) {
    connection().request_pause(true, pause);
    return true;
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
