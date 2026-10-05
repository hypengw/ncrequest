export module ncrequest:unix_duplex;
export import :options;
export import rstd;

using namespace rstd::prelude;

export namespace ncrequest
{
struct DuplexOptions {
    rstd::time::Duration connect_timeout { rstd::time::Duration::from_secs(u64(10)) };
    rstd::time::Duration io_timeout { rstd::time::Duration::from_secs(u64(30)) };
};

class UnixDuplex {
    class Impl;
    Box<Impl> impl_;

public:
    explicit UnixDuplex(Box<Impl> impl);
    UnixDuplex(const UnixDuplex&)                    = delete;
    auto operator=(const UnixDuplex&) -> UnixDuplex& = delete;
    ~UnixDuplex();

    static auto make(Endpoint endpoint, DuplexOptions options = {}) -> Result<Box<UnixDuplex>>;
    // Await once before IO. Dropping a pending operation cancels the connection.
    auto connected() -> rstd::async::Completion<Result<empty>>;
    // One read and one write may be pending together; empty reads mean EOF.
    auto read() -> rstd::async::Completion<Result<rstd::bytes::Bytes>>;
    // Writes are bounded to 64 KiB and complete only after all bytes are sent.
    auto write(rstd::bytes::Bytes bytes) -> rstd::async::Completion<Result<usize>>;
    auto close_input() -> rstd::async::Completion<Result<empty>>;
    void cancel();
};
} // namespace ncrequest
