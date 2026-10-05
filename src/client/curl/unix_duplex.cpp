module;
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
module ncrequest;
import :unix_duplex;
import ncrequest.curl;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace curl;
using rstd::async::Completion;
using rstd::async::CompletionHandle;
using rstd::bytes::Bytes;
using rstd::time::Instant;

namespace ncrequest
{
class UnixDuplex::Impl {
    template<class T>
    struct Pending {
        CompletionHandle<Result<T>> completion;
        Instant                     started { Instant::now() };
    };
    struct Write {
        Pending<usize> pending;
        Bytes          bytes;
        usize          offset {};
    };
    struct Fields {
        bool                              ready {}, stopped {}, input_closed {}, output_eof {};
        Option<Completion<Result<empty>>> connected;
        Option<Pending<empty>>            connecting;
        Option<Pending<Bytes>>            reading;
        Option<Write>                     writing;
        Option<Pending<empty>>            closing;
    };
    Endpoint                               endpoint_;
    DuplexOptions                          options_;
    rstd::sync::Mutex<Fields>              fields_ { Fields {} };
    rstd::sync::atomic::Atomic<bool>       cancelled_ { false };
    int                                    wake_ { -1 };
    Option<rstd::thread::JoinHandle<void>> worker_;
    CURL*                                  easy_ {};
    CURLM*                                 multi_ {};
    bool                                   added_ {};
    curl_socket_t                          socket_ { -1 };

    void wake() {
        uint64_t value = 1;
        while (::write(wake_, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
    }
    void drain() {
        uint64_t value;
        while (::read(wake_, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
    }
    static auto io_error() -> Error { return Error::Io(rstd::io::error::Error::last_os_error()); }
    template<class T>
    static auto rejected(Error error) -> Completion<Result<T>> {
        auto pair = Completion<Result<T>>::make().unwrap();
        (void)pair.template get<1>().complete(Err(rstd::move(error)));
        return rstd::move(pair.template get<0>());
    }
    template<class T, class R>
    static auto finish(Option<Pending<T>>& pending, R result) -> void {
        if (pending.is_some()) (void)pending.take()->completion.complete(rstd::move(result));
    }
    void fail(Error error) {
        auto fields     = fields_.lock().unwrap();
        fields->stopped = true;
        fields->ready   = false;
        finish(fields->connecting, Err(error.clone()));
        finish(fields->reading, Err(error.clone()));
        finish(fields->closing, Err(error.clone()));
        if (fields->writing.is_some())
            (void)fields->writing.take()->pending.completion.complete(Err(rstd::move(error)));
    }
    auto connect() -> Result<empty> {
        easy_  = curl_easy_init();
        multi_ = curl_multi_init();
        if (! easy_ || ! multi_) return Err(Error::InvalidState("curl allocation failed"));
        auto path = rstd::ffi::CString::from_vec_unchecked(
            Vec<u8>::from(endpoint_.socket_path()->as_os_str().as_encoded_bytes()));
        CURLcode code = CURLcode::CURLE_OK;
        auto     set  = [&](CURLoption option, auto value) {
            if (code == CURLcode::CURLE_OK) code = curl_easy_setopt(easy_, option, value);
        };
        set(CURLoption::CURLOPT_URL, "http://localhost/");
        set(CURLoption::CURLOPT_PROXY, "");
        set(CURLoption::CURLOPT_UNIX_SOCKET_PATH, path.as_ptr());
        set(CURLoption::CURLOPT_CONNECT_ONLY, 1L);
        set(CURLoption::CURLOPT_NOSIGNAL, 1L);
        if (code != CURLcode::CURLE_OK) return Err(rstd::into<Error>(code));
        auto added = curl_multi_add_handle(multi_, easy_);
        if (added != CURLMcode::CURLM_OK)
            return Err(rstd::into<Error>(CurlMultiError::Multi(added)));
        added_       = true;
        auto started = Instant::now();
        for (;;) {
            if (cancelled_.load()) return Err(Error::Canceled());
            {
                auto fields = fields_.lock().unwrap();
                if (fields->connecting->completion.is_closed()) return Err(Error::Canceled());
            }
            if (started.elapsed() >= options_.connect_timeout) return Err(Error::Timeout());
            int  running {};
            auto result = curl_multi_perform(multi_, &running);
            if (result != CURLMcode::CURLM_OK)
                return Err(rstd::into<Error>(CurlMultiError::Multi(result)));
            int remaining {};
            while (auto* message = curl_multi_info_read(multi_, &remaining)) {
                if (message->msg != CURLMSG::CURLMSG_DONE || message->easy_handle != easy_)
                    continue;
                if (message->data.result != CURLcode::CURLE_OK)
                    return Err(rstd::into<Error>(message->data.result));
                code = curl_easy_getinfo(easy_, CURLINFO::CURLINFO_ACTIVESOCKET, &socket_);
                if (code != CURLcode::CURLE_OK) return Err(rstd::into<Error>(code));
                if (socket_ < 0) return Err(Error::InvalidState("curl connection has no socket"));
                return Ok(empty {});
            }
            curl_waitfd wake { wake_, 1, 0 };
            result = curl_multi_poll(multi_, &wake, 1, 10, nullptr);
            if (result != CURLMcode::CURLM_OK)
                return Err(rstd::into<Error>(CurlMultiError::Multi(result)));
            if (wake.revents) drain();
        }
    }
    auto progress() -> Result<short> {
        auto fields  = fields_.lock().unwrap();
        auto expired = [&](auto& pending) {
            return pending.started.elapsed() >= options_.io_timeout;
        };
        if ((fields->reading.is_some() && fields->reading->completion.is_closed()) ||
            (fields->writing.is_some() && fields->writing->pending.completion.is_closed()) ||
            (fields->closing.is_some() && fields->closing->completion.is_closed()))
            return Err(Error::Canceled());
        if ((fields->reading.is_some() && expired(*fields->reading)) ||
            (fields->writing.is_some() && expired(fields->writing->pending)) ||
            (fields->closing.is_some() && expired(*fields->closing)))
            return Err(Error::Timeout());
        short events {};
        if (fields->reading.is_some()) {
            array<u8, 16384> bytes {};
            rstd::size_t     count {};
            auto             code = curl_easy_recv(easy_, bytes.data(), 16384, &count);
            if (code == CURLcode::CURLE_AGAIN)
                events |= POLLIN;
            else if (code != CURLcode::CURLE_OK)
                return Err(rstd::into<Error>(code));
            else {
                fields->output_eof = count == 0;
                finish(fields->reading,
                       Ok(Bytes::copy_from_slice(
                           slice<u8>::from_raw_parts(bytes.data(), usize(count)))));
            }
        }
        if (fields->writing.is_some()) {
            auto&        write = *fields->writing;
            rstd::size_t count {};
            auto         code =
                curl_easy_send(easy_,
                               write.bytes.as_slice().as_raw_ptr() + write.offset.to_primitive(),
                               (write.bytes.len() - write.offset).to_primitive(),
                               &count);
            if (code == CURLcode::CURLE_AGAIN)
                events |= POLLOUT;
            else if (code != CURLcode::CURLE_OK)
                return Err(rstd::into<Error>(code));
            else {
                if (count == 0) return Err(Error::InvalidState("curl write made no progress"));
                write.offset += usize(count);
                if (write.offset == write.bytes.len()) {
                    auto done = fields->writing.take().unwrap();
                    (void)done.pending.completion.complete(Ok(done.offset));
                } else
                    events |= POLLOUT;
            }
        }
        if (fields->closing.is_some()) {
            if (::shutdown(socket_, SHUT_WR) != 0) return Err(io_error());
            fields->input_closed = true;
            finish(fields->closing, Ok(empty {}));
        }
        return Ok(events);
    }
    void run() {
        auto connected = connect();
        if (connected.is_err()) {
            fail(rstd::move(connected).unwrap_err());
            return;
        }
        {
            auto fields   = fields_.lock().unwrap();
            fields->ready = true;
            finish(fields->connecting, Ok(empty {}));
        }
        while (! cancelled_.load()) {
            auto events = progress();
            if (events.is_err()) {
                fail(rstd::move(events).unwrap_err());
                return;
            }
            pollfd descriptors[2] { { wake_, POLLIN, 0 }, { socket_, *events, 0 } };
            auto   result = ::poll(descriptors, *events ? 2 : 1, *events ? 10 : -1);
            if (result < 0 && errno != EINTR) {
                fail(io_error());
                return;
            }
            if (descriptors[0].revents) drain();
        }
        fail(Error::Canceled());
    }
    void release() {
        if (added_) (void)curl_multi_remove_handle(multi_, easy_);
        if (easy_) curl_easy_cleanup(easy_);
        if (multi_) curl_multi_cleanup(multi_);
        easy_  = nullptr;
        multi_ = nullptr;
    }

public:
    Impl(Endpoint endpoint, DuplexOptions options)
        : endpoint_(rstd::move(endpoint)), options_(options) {}
    ~Impl() {
        cancel();
        if (worker_.is_some()) (void)rstd::move(worker_.take().unwrap()).join();
        if (wake_ >= 0) ::close(wake_);
    }
    auto start() -> Result<empty> {
        wake_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_ < 0) return Err(io_error());
        auto pair = Completion<Result<empty>>::make().unwrap();
        {
            auto fields        = fields_.lock().unwrap();
            fields->connected  = Some(rstd::move(pair.get<0>()));
            fields->connecting = Some(Pending<empty> { rstd::move(pair.get<1>()) });
        }
        auto spawned = rstd::thread::spawn([this] {
            run();
            release();
        });
        if (spawned.is_err()) return Err(Error::Io(rstd::move(spawned).unwrap_err()));
        worker_ = Some(rstd::move(spawned).unwrap());
        return Ok(empty {});
    }
    auto connected() -> Completion<Result<empty>> {
        auto fields = fields_.lock().unwrap();
        if (fields->connected.is_none())
            return rejected<empty>(Error::InvalidState("connection completion already taken"));
        return fields->connected.take().unwrap();
    }
    auto read() -> Completion<Result<Bytes>> {
        auto fields = fields_.lock().unwrap();
        if (! fields->ready || cancelled_.load()) return rejected<Bytes>(Error::Canceled());
        if (fields->reading.is_some())
            return rejected<Bytes>(Error::InvalidState("read already pending"));
        auto pair = Completion<Result<Bytes>>::make().unwrap();
        if (fields->output_eof)
            (void)pair.get<1>().complete(Ok(Bytes {}));
        else {
            fields->reading = Some(Pending<Bytes> { rstd::move(pair.get<1>()) });
            wake();
        }
        return rstd::move(pair.get<0>());
    }
    auto write(Bytes bytes) -> Completion<Result<usize>> {
        auto fields = fields_.lock().unwrap();
        if (! fields->ready || cancelled_.load()) return rejected<usize>(Error::Canceled());
        if (fields->input_closed || fields->closing.is_some() || fields->writing.is_some())
            return rejected<usize>(Error::InvalidState("input closed or write pending"));
        if (bytes.len() > usize(65536))
            return rejected<usize>(Error::InvalidState("write exceeds 64 KiB"));
        auto pair = Completion<Result<usize>>::make().unwrap();
        if (bytes.is_empty())
            (void)pair.get<1>().complete(Ok(usize()));
        else {
            fields->writing = Some(Write { { rstd::move(pair.get<1>()) }, rstd::move(bytes) });
            wake();
        }
        return rstd::move(pair.get<0>());
    }
    auto close_input() -> Completion<Result<empty>> {
        auto fields = fields_.lock().unwrap();
        if (! fields->ready || cancelled_.load()) return rejected<empty>(Error::Canceled());
        if (fields->writing.is_some() || fields->closing.is_some())
            return rejected<empty>(Error::InvalidState("input operation pending"));
        auto pair = Completion<Result<empty>>::make().unwrap();
        if (fields->input_closed)
            (void)pair.get<1>().complete(Ok(empty {}));
        else {
            fields->closing = Some(Pending<empty> { rstd::move(pair.get<1>()) });
            wake();
        }
        return rstd::move(pair.get<0>());
    }
    void cancel() {
        cancelled_.store(true);
        if (wake_ >= 0) wake();
    }
};

UnixDuplex::UnixDuplex(Box<Impl> impl): impl_(rstd::move(impl)) {}
UnixDuplex::~UnixDuplex() = default;
auto UnixDuplex::make(Endpoint endpoint, DuplexOptions options) -> Result<Box<UnixDuplex>> {
    if (endpoint.socket_path().is_none() || options.connect_timeout.is_zero() ||
        options.io_timeout.is_zero())
        return Err(Error::InvalidState("Unix endpoint and positive deadlines required"));
    auto initialized = curl_init();
    if (initialized.is_err()) return Err(rstd::into<Error>(initialized.unwrap_err()));
    auto impl    = Box<Impl>::make(rstd::move(endpoint), options);
    auto started = impl->start();
    if (started.is_err()) return Err(rstd::move(started).unwrap_err());
    return Ok(Box<UnixDuplex>::make(rstd::move(impl)));
}
auto UnixDuplex::connected() -> Completion<Result<empty>> { return impl_->connected(); }
auto UnixDuplex::read() -> Completion<Result<Bytes>> { return impl_->read(); }
auto UnixDuplex::write(Bytes bytes) -> Completion<Result<usize>> {
    return impl_->write(rstd::move(bytes));
}
auto UnixDuplex::close_input() -> Completion<Result<empty>> { return impl_->close_input(); }
void UnixDuplex::cancel() { impl_->cancel(); }
} // namespace ncrequest
