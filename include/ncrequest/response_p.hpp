#pragma once

#include <atomic>
#include <asio/streambuf.hpp>

#include "ncrequest/response.hpp"
#include "ncrequest/session.hpp"

namespace ncrequest
{

class Connection;
class Response::Private {
public:
    Private(Response*, const Request&, Operation, rc<Session>) noexcept;

    inline Response*       q_func() { return static_cast<Response*>(m_q); }
    inline const Response* q_func() const { return static_cast<const Response*>(m_q); }
    friend class Response;

    void set_share(const std::optional<SessionShare>& share) { m_share = share; }

private:
    Response* m_q;
    Request   m_req;

    Operation m_operation;
    bool      m_finished;

    asio::streambuf             m_send_buffer;
    rc<Connection>              m_connect;
    std::optional<SessionShare> m_share;

    std::pmr::polymorphic_allocator<char> m_allocator;
};

} // namespace ncrequest
