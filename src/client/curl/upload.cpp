module ncrequest;
import :client_curl_connection;

using namespace rstd::prelude;
using rstd::async::spawn;
using rstd::async::yield_now;
using rstd::sync::Arc;

namespace ncrequest::client::curl
{

static auto pump_upload(Arc<Connection> connection, BodyStream::Callback next, Option<u64> length)
    -> coro<void> {
    u64 count {};
    while (length.is_none() || count < *length) {
        if (connection->is_finished()) co_return;
        auto part = co_await next();
        if (part.is_err()) {
            connection->fail_upload(rstd::move(part).unwrap_err());
            co_return;
        }
        if (part->is_none()) {
            if (length.is_some()) {
                connection->fail_upload(
                    Error::Protocol(ProtocolError::BodyLengthMismatch, nullptr));
                co_return;
            }
            break;
        }
        auto bytes = part->take().unwrap();
        auto size  = u64(bytes.size().to_primitive());
        if (size > u64::MAX - count || (length.is_some() && size > *length - count)) {
            connection->fail_upload(Error::Protocol(ProtocolError::BodyLengthMismatch, nullptr));
            co_return;
        }
        count += size;
        while (bytes.size() != usize()) {
            auto written = co_await connection->write_some(bytes);
            if (written.error.is_some()) {
                connection->fail_upload(written.error.take().unwrap());
                co_return;
            }
            if (written.eof) co_return;
        }
        co_await yield_now();
    }
    connection->finish_upload();
}

void Connection::start_upload() {
    auto& stream = request().body().stream();
    if (stream.is_none()) return;
    auto task = spawn(pump_upload(get_arc(), stream->next.clone(), stream->size));
    {
        auto lock = RawMutexGuard { m_mutex };
        if (m_state != State::Finished && m_state != State::Canceled) {
            m_upload_task = Some(rstd::move(task));
            return;
        }
    }
    task.abort();
}

} // namespace ncrequest::client::curl
