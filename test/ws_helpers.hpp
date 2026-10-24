#pragma once

template<class Connection>
auto write_ws(const Connection& connection,liwebsocketo::Message message)->rstd::async::coro<ncrequest::WebSocketResult<rstd::empty>>{
    auto accepted=connection.send(rstd::move(message));
    if(accepted.is_err())co_return rstd::Err(rstd::move(accepted).unwrap_err().error);
    auto pending=rstd::move(accepted).unwrap();
    co_return co_await pending.written();
}
inline auto read_ws(const ncrequest::WebSocketConnection& connection)->rstd::async::coro<ncrequest::WebSocketResult<liwebsocketo::Message>>{
    auto pending=connection.receive();
    co_return co_await pending.message();
}
