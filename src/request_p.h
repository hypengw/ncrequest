#pragma once

#include "request.h"
#include "macro.h"

namespace ncrequest
{

class Request::Private {
public:
    C_DECLARE_PUBLIC(Request, m_q)
    Private(Request* q);
    ~Private();

private:
    Request* m_q;
    URI      m_uri;
    Header   m_header;

    RequestOpts::to<std::tuple> m_opts;
};
} // namespace ncrequest
