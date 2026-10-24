module;
#include <rstd/macro.hpp>
#if RSTD_OS_WINDOWS
#    include <winsock2.h>
#else
#    include <sys/socket.h>
#endif
#include <curl/curl.h>
#include <string.h>
module ncrequest;
import :client.curl.backend;
import :client.io;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace ncrequest::client::curl
{
namespace lf = lifetchto;
namespace h  = lifetchto::http;
auto failure(CURLcode code) -> lf::Error {
    return lf::Error::Extension("curl"_Str,
                                Arc<rstd::dyn<rstd::error::Error>>::make(Error(i32(code))));
}
auto failure(CURLMcode code) -> lf::Error {
    return lf::Error::Extension("curl"_Str,
                                Arc<rstd::dyn<rstd::error::Error>>::make(Error::multi(i32(code))));
}
auto configure_tls(CURL* easy, const h::TlsOptions& options, bool proxy = false)
    -> lf::Result<empty> {
    CURLcode code = CURLE_OK;
    auto     set  = [&](CURLoption key, auto value) {
        if (code == CURLE_OK) code = curl_easy_setopt(easy, key, value);
    };
    set(proxy ? CURLOPT_PROXY_SSL_VERIFYPEER : CURLOPT_SSL_VERIFYPEER,
        options.verification != h::TlsVerification::Disabled ? 1L : 0L);
    set(proxy ? CURLOPT_PROXY_SSL_VERIFYHOST : CURLOPT_SSL_VERIFYHOST,
        options.verification == h::TlsVerification::Strict ? 2L : 0L);
    if (! options.use_system_roots) {
        set(proxy ? CURLOPT_PROXY_CAINFO : CURLOPT_CAINFO, static_cast<const char*>(nullptr));
        set(proxy ? CURLOPT_PROXY_CAPATH : CURLOPT_CAPATH, static_cast<const char*>(nullptr));
    }
    Vec<u8> certificates;
    for (const auto& certificate : options.ca_pem) {
        certificates.extend_from_slice(certificate.as_slice());
        certificates.push(u8('\n'));
    }
    if (! certificates.is_empty()) {
        curl_blob ca { const_cast<void*>(
                           static_cast<const void*>(certificates.as_slice().as_raw_ptr())),
                       certificates.len().to_primitive(),
                       CURL_BLOB_COPY };
        set(proxy ? CURLOPT_PROXY_CAINFO_BLOB : CURLOPT_CAINFO_BLOB, &ca);
    }
    if (options.identity.is_some()) {
        const auto& identity = *options.identity;
        curl_blob   certificate { const_cast<void*>(static_cast<const void*>(
                                      identity.certificate_chain.as_slice().as_raw_ptr())),
                                  identity.certificate_chain.len().to_primitive(),
                                  CURL_BLOB_COPY };
        curl_blob key { const_cast<void*>(
                            static_cast<const void*>(identity.private_key.as_slice().as_raw_ptr())),
                        identity.private_key.len().to_primitive(),
                        CURL_BLOB_COPY };
        set(proxy ? CURLOPT_PROXY_SSLCERTTYPE : CURLOPT_SSLCERTTYPE, "PEM");
        set(proxy ? CURLOPT_PROXY_SSLKEYTYPE : CURLOPT_SSLKEYTYPE, "PEM");
        set(proxy ? CURLOPT_PROXY_SSLCERT_BLOB : CURLOPT_SSLCERT_BLOB, &certificate);
        set(proxy ? CURLOPT_PROXY_SSLKEY_BLOB : CURLOPT_SSLKEY_BLOB, &key);
    }
    if (code != CURLE_OK) return Err(failure(code));
    return Ok(empty {});
}
auto curl_tcp_capabilities() -> h::TcpCapabilities {
    return { true, true, rstd::net::TcpStream::keepalive_timing_supported() };
}
auto configure_tcp(CURL* easy, const h::TcpOptions& options) -> lf::Result<empty> {
    CURLcode code = CURLE_OK;
    auto     set  = [&](CURLoption key, long value) {
        if (code == CURLE_OK) code = curl_easy_setopt(easy, key, value);
    };
    if (options.nodelay.is_some()) set(CURLOPT_TCP_NODELAY, *options.nodelay ? 1L : 0L);
    if (options.keepalive.is_some()) {
        set(CURLOPT_TCP_KEEPALIVE, options.keepalive->enabled ? 1L : 0L);
        if (options.keepalive->idle.is_some())
            set(CURLOPT_TCP_KEEPIDLE, long(options.keepalive->idle->as_secs().to_primitive()));
        if (options.keepalive->interval.is_some())
            set(CURLOPT_TCP_KEEPINTVL, long(options.keepalive->interval->as_secs().to_primitive()));
    }
    if (code != CURLE_OK) return Err(failure(code));
    return Ok(empty {});
}
auto proxy_capabilities() -> h::ProxyCapabilities {
    auto* version = curl_version_info(CURLVERSION_NOW);
    return { .http            = true,
             .https           = bool(version->features & CURL_VERSION_HTTPS_PROXY),
             .socks4          = true,
             .socks4a         = true,
             .socks5          = true,
             .socks5h         = true,
             .system_snapshot = true };
}
auto configure_proxy(CURL* easy, const Option<h::ProxyEndpoint>& proxy, bool tunnel,
                     const h::TlsOptions& tls) -> lf::Result<empty> {
    CURLcode code = CURLE_OK;
    auto     set  = [&](CURLoption key, auto value) {
        if (code == CURLE_OK) code = curl_easy_setopt(easy, key, value);
    };
    set(CURLOPT_NOPROXY, "");
    if (proxy.is_none())
        set(CURLOPT_PROXY, "");
    else {
        auto url =
            rstd::ffi::CString::from_vec_unchecked(Vec<u8>::from(proxy->url().as_ref().as_bytes()));
        set(CURLOPT_PROXY, url.as_ptr());
        set(CURLOPT_PROXYPORT, long(proxy->port().to_primitive()));
        long type {};
        switch (proxy->type()) {
        case h::ProxyType::Http: type = CURLPROXY_HTTP; break;
        case h::ProxyType::Https: type = CURLPROXY_HTTPS; break;
        case h::ProxyType::Socks4: type = CURLPROXY_SOCKS4; break;
        case h::ProxyType::Socks4a: type = CURLPROXY_SOCKS4A; break;
        case h::ProxyType::Socks5: type = CURLPROXY_SOCKS5; break;
        case h::ProxyType::Socks5h: type = CURLPROXY_SOCKS5_HOSTNAME; break;
        }
        set(CURLOPT_PROXYTYPE, type);
        set(CURLOPT_HTTPPROXYTUNNEL, tunnel ? 1L : 0L);
        set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        if (proxy->type() == h::ProxyType::Https) {
            auto configured = configure_tls(easy, tls, true);
            if (configured.is_err()) return Err(rstd::move(configured).unwrap_err());
        }
    }
    if (code != CURLE_OK) return Err(failure(code));
    return Ok(empty {});
}
struct RawFields {
    Option<lf::DuplexChannel>  channel;
    Option<lf::ResourcePermit> permit;
    Option<lf::Error>          error;
    bool                       dispatched {}, ready {}, done {};
};
struct RawExchange {
    h::DuplexRequest             request;
    Option<h::ProxyEndpoint>     proxy;
    rstd::sync::Mutex<RawFields> fields { RawFields {} };
    lf::Cancellation             stopped;
    lf::Event                    changed;
    explicit RawExchange(h::DuplexRequest request, Option<h::ProxyEndpoint> proxy)
        : request(rstd::move(request)), proxy(rstd::move(proxy)) {}
    void stop(lf::StopReason reason = lf::StopReason::Canceled) const {
        stopped.cancel(reason);
        auto f = fields.lock().unwrap();
        if (f->channel.is_some()) f->channel->request_stop(reason);
        changed.notify();
    }
    void fail(lf::Error error) const {
        auto f = fields.lock().unwrap();
        if (f->error.is_none()) f->error = Some(error.clone());
        if (f->channel.is_some()) f->channel->fail(rstd::move(error));
        stopped.cancel(lf::StopReason::Canceled);
        changed.notify();
    }
    void complete() const {
        fields.lock().unwrap()->done = true;
        changed.notify();
    }
};
struct BackendFields {
    Vec<Arc<Exchange>>    queued;
    Vec<Arc<RawExchange>> raw;
    bool                  closed {};
    usize                 handles { 1 };
};
struct BackendState {
    Options                          options;
    h::BackendIdentity               identity;
    rstd::sync::Mutex<BackendFields> fields { BackendFields {} };
    Option<lf::TaskControl>          worker;
    explicit BackendState(Options options): options(rstd::move(options)) {}
};
void stop(const Arc<BackendState>& state) { state->fields.lock().unwrap()->closed = true; }
void fail_pending(const Arc<BackendState>& state, const lf::Error& error) {
    Vec<Arc<Exchange>>    queued;
    Vec<Arc<RawExchange>> raw;
    {
        auto f    = state->fields.lock().unwrap();
        f->closed = true;
        queued    = rstd::move(f->queued);
        raw       = rstd::move(f->raw);
    }
    for (const auto& exchange : queued) {
        exchange->fail(error.clone());
        exchange->complete();
    }
    for (const auto& exchange : raw) {
        exchange->fail(error.clone());
        exchange->complete();
    }
}
struct Active {
    CURL*                         easy {};
    curl_slist*                   headers {};
    curl_slist*                   resolve {};
    curl_slist*                   trailers {};
    Arc<Exchange>                 exchange;
    lihttpto::ResponseHeadDecoder decoder;
    bool                          head {}, paused {}, upload_paused {};
    usize                         metadata {};
    usize                         metadata_limit;
    Active(Arc<Exchange> exchange, usize limit)
        : exchange(rstd::move(exchange)),
          decoder(this->exchange->prepared.parts().method.clone(), limit),
          metadata_limit(limit) {}
    ~Active() { cleanup(); }
    void cleanup() {
        if (easy) curl_easy_cleanup(easy);
        curl_slist_free_all(headers);
        curl_slist_free_all(resolve);
        curl_slist_free_all(trailers);
        easy    = nullptr;
        headers = resolve = trailers = nullptr;
    }
    static auto read(char* bytes, size_t, size_t maximum, void* opaque) -> size_t {
        auto& self = *static_cast<Active*>(opaque);
        auto  part = self.exchange->outgoing->try_next(usize(maximum));
        if (part.is_err()) {
            self.exchange->fail(rstd::move(part).unwrap_err());
            return CURL_READFUNC_ABORT;
        }
        if (part->is_none()) {
            self.upload_paused = true;
            self.exchange->demand();
            return CURL_READFUNC_PAUSE;
        }
        auto value = part->take().unwrap();
        if (value.is_none()) return 0;
        ::memcpy(bytes, value->as_slice().as_raw_ptr(), value->len().to_primitive());
        return value->len().to_primitive();
    }
    static auto write(char* bytes, size_t size, size_t count, void* opaque) -> size_t {
        auto& self   = *static_cast<Active*>(opaque);
        auto  length = size * count;
        auto  sent   = self.exchange->incoming->try_send(Bytes::copy_from_slice(
            slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(bytes), usize(length))));
        if (sent.is_ok()) return length;
        if (sent.unwrap_err().is_Capacity()) {
            self.paused = true;
            return CURL_WRITEFUNC_PAUSE;
        }
        self.exchange->fail(rstd::move(sent).unwrap_err());
        return CURL_WRITEFUNC_ERROR;
    }
    static auto header(char* bytes, size_t size, size_t count, void* opaque) -> size_t {
        auto& self   = *static_cast<Active*>(opaque);
        auto  length = size * count;
        auto  raw =
            slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(bytes), usize(length));
        if (! self.head) {
            auto decoded = self.decoder.feed(raw);
            if (decoded.is_err()) {
                self.exchange->fail(h::protocol_error("invalid curl response head"_Str,
                                                      Some(decoded.unwrap_err())));
                return 0;
            }
            if (decoded->is_some() && ! (*decoded)->informational) {
                self.head                                   = true;
                self.exchange->fields.lock().unwrap()->head = Some(rstd::move((**decoded).head));
                self.exchange->notify();
            }
        } else if (length > 2) {
            self.metadata += usize(length);
            if (self.metadata + self.decoder.metadata_bytes() > self.metadata_limit) {
                self.exchange->fail(lf::Error::TooLarge());
                return 0;
            }
            auto parsed = lihttpto::Header::parse_line(
                slice<u8>::from_raw_parts(raw.as_raw_ptr(), raw.len() - usize(2)));
            if (parsed.is_err()) {
                self.exchange->fail(h::protocol_error("invalid curl trailer"_Str));
                return 0;
            }
            self.exchange->fields.lock().unwrap()->trailers.push(rstd::move(parsed).unwrap());
        }
        return length;
    }
    static auto trailer(curl_slist** result, void* opaque) -> int {
        auto& self    = *static_cast<Active*>(opaque);
        *result       = self.trailers;
        self.trailers = nullptr;
        return CURL_TRAILERFUNC_OK;
    }
    auto configure(const Options& options) -> lf::Result<empty> {
        easy = curl_easy_init();
        if (! easy) return Err(failure(CURLE_FAILED_INIT));
        CURLcode option_error = CURLE_OK;
        auto     set          = [&](CURLoption key, auto value) -> bool {
            option_error = curl_easy_setopt(easy, key, value);
            return option_error == CURLE_OK;
        };
        auto append = [](curl_slist*& list, const char* value) -> bool {
            auto* next = curl_slist_append(list, value);
            if (! next) return false;
            list = next;
            return true;
        };
        auto& parts = exchange->prepared.parts();
        auto  url =
            rstd::ffi::CString::from_vec_unchecked(Vec<u8>::from(parts.url.as_ref().as_bytes()));
        auto method =
            rstd::ffi::CString::from_vec_unchecked(Vec<u8>::from(parts.method.as_ref().as_bytes()));
        if (! set(CURLOPT_URL, url.as_ptr()) || ! set(CURLOPT_CUSTOMREQUEST, method.as_ptr()) ||
            ! set(CURLOPT_NOSIGNAL, 1L) || ! set(CURLOPT_FOLLOWLOCATION, 0L) ||
            ! set(CURLOPT_FAILONERROR, 0L) || ! set(CURLOPT_PROXY, "") ||
            ! set(CURLOPT_HTTP_VERSION, long(CURL_HTTP_VERSION_1_1)) ||
            ! set(CURLOPT_CONNECTTIMEOUT_MS,
                  long(options.connect_timeout.as_millis().to_primitive())) ||
            ! set(CURLOPT_WRITEFUNCTION, &write) || ! set(CURLOPT_WRITEDATA, this) ||
            ! set(CURLOPT_HEADERFUNCTION, &header) || ! set(CURLOPT_HEADERDATA, this) ||
            ! set(CURLOPT_READFUNCTION, &read) || ! set(CURLOPT_READDATA, this) ||
            ! set(CURLOPT_PRIVATE, this))
            return Err(failure(option_error));
        auto tls = configure_tls(easy, options.tls);
        if (tls.is_err()) return Err(rstd::move(tls).unwrap_err());
        auto proxy = configure_proxy(easy, exchange->prepared.proxy(), false, options.proxy_tls);
        if (proxy.is_err()) return Err(rstd::move(proxy).unwrap_err());
        auto tcp = configure_tcp(easy, options.tcp);
        if (tcp.is_err()) return Err(rstd::move(tcp).unwrap_err());
        if (options.low_speed.is_some()) {
            if (! set(CURLOPT_LOW_SPEED_LIMIT,
                      long(options.low_speed->bytes_per_second.to_primitive())) ||
                ! set(CURLOPT_LOW_SPEED_TIME,
                      long(options.low_speed->window.as_secs().to_primitive())))
                return Err(failure(option_error));
        }
        for (const auto& field : parts.headers) {
            Vec<u8> raw;
            raw.extend_from_slice(field.name.as_str().as_bytes());
            raw.extend_from_slice(": "_str.as_bytes());
            raw.extend_from_slice(field.value.as_slice());
            raw.push(u8());
            if (! append(headers, reinterpret_cast<const char*>(raw.as_slice().as_raw_ptr())))
                return Err(failure(CURLE_OUT_OF_MEMORY));
        }
        if ((! parts.headers.contains("expect"_str) && ! append(headers, "Expect:")) ||
            (! parts.headers.contains("accept"_str) && ! append(headers, "Accept:")))
            return Err(failure(CURLE_OUT_OF_MEMORY));
        if (! set(CURLOPT_HTTPHEADER, headers)) return Err(failure(option_error));
        auto length = exchange->prepared.upload().length;
        if (parts.method.as_ref() == "HEAD"_str) {
            if (! set(CURLOPT_NOBODY, 1L)) return Err(failure(option_error));
        } else if (length.is_none() || *length != u64() ||
                   exchange->prepared.upload().trailers.is_some()) {
            if (! set(CURLOPT_UPLOAD, 1L) ||
                ! set(CURLOPT_INFILESIZE_LARGE,
                      length.is_some() ? curl_off_t(length->to_primitive()) : curl_off_t(-1)))
                return Err(failure(option_error));
        }
        auto& metadata = exchange->prepared.upload();
        if (metadata.trailers.is_some()) {
            for (const auto& field : *metadata.trailers) {
                Vec<u8> raw;
                raw.extend_from_slice(field.name.as_str().as_bytes());
                raw.extend_from_slice(": "_str.as_bytes());
                raw.extend_from_slice(field.value.as_slice());
                raw.push(u8());
                if (! append(trailers, reinterpret_cast<const char*>(raw.as_slice().as_raw_ptr())))
                    return Err(failure(CURLE_OUT_OF_MEMORY));
            }
            if (! set(CURLOPT_TRAILERFUNCTION, &trailer) || ! set(CURLOPT_TRAILERDATA, this) ||
                ! set(CURLOPT_INFILESIZE_LARGE, curl_off_t(-1)))
                return Err(failure(option_error));
        }
        if (exchange->prepared.target().route.is_some()) {
            const auto& route = **exchange->prepared.target().route;
            if (route.unix_path().is_some()) {
                Vec<u8> path;
                path.extend_from_slice(route.unix_path()->as_slice());
                path.push(u8());
                if (! set(CURLOPT_UNIX_SOCKET_PATH,
                          reinterpret_cast<const char*>(path.as_slice().as_raw_ptr())))
                    return Err(failure(option_error));
            } else {
                auto origin = parts.url.http_origin().unwrap();
                auto record =
                    rstd::format("{}:{}:{}", origin.host, origin.port, route.addresses()[usize()]);
                auto token = rstd::ffi::CString::from_vec_unchecked(
                    Vec<u8>::from(record.as_str().as_bytes()));
                if (! append(resolve, token.as_ptr())) return Err(failure(CURLE_OUT_OF_MEMORY));
                if (! set(CURLOPT_CONNECT_TO, resolve)) return Err(failure(option_error));
            }
        }
        return Ok(empty {});
    }
};
struct RawActive {
    CURL*                   easy {};
    curl_slist*             route {};
    Arc<RawExchange>        exchange;
    lf::DuplexChannel       channel;
    Option<lf::DuplexWrite> writing;
    usize                   offset;
    bool                    connected {}, eof {};
    explicit RawActive(Arc<RawExchange> exchange)
        : exchange(rstd::move(exchange)),
          channel(this->exchange->fields.lock().unwrap()->channel->clone()) {}
    ~RawActive() { cleanup(); }
    void cleanup() {
        writing = None();
        if (easy) curl_easy_cleanup(easy);
        easy = nullptr;
        curl_slist_free_all(route);
        route = nullptr;
    }
    auto configure(const Options& options) -> lf::Result<empty> {
        easy = curl_easy_init();
        if (! easy) return Err(failure(CURLE_FAILED_INIT));
        CURLcode code = CURLE_OK;
        auto     set  = [&](CURLoption key, auto value) {
            if (code == CURLE_OK) code = curl_easy_setopt(easy, key, value);
        };
        auto url = rstd::ffi::CString::from_vec_unchecked(
            Vec<u8>::from(exchange->request.url.as_ref().as_bytes()));
        set(CURLOPT_URL, url.as_ptr());
        set(CURLOPT_PROXY, "");
        set(CURLOPT_CONNECT_ONLY, 1L);
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_HTTP_VERSION, long(CURL_HTTP_VERSION_1_1));
        set(CURLOPT_CONNECTTIMEOUT_MS, long(options.connect_timeout.as_millis().to_primitive()));
        auto tls = configure_tls(easy, options.tls);
        if (tls.is_err()) return Err(rstd::move(tls).unwrap_err());
        auto proxy = configure_proxy(easy, exchange->proxy, true, options.proxy_tls);
        if (proxy.is_err()) return Err(rstd::move(proxy).unwrap_err());
        auto tcp = configure_tcp(easy, options.tcp);
        if (tcp.is_err()) return Err(rstd::move(tcp).unwrap_err());
        if (exchange->request.route.route.is_some()) {
            const auto& target = **exchange->request.route.route;
            auto        value  = target.unix_path().is_some()
                                     ? rstd::ffi::CString::from_vec_unchecked(
                                           Vec<u8>::from(target.unix_path()->as_slice()))
                                     : rstd::ffi::CString::from_vec_unchecked(Vec<u8>::from(
                                           rstd::format("{}:{}:{}",
                                                        exchange->request.url.http_origin()->host,
                                                        exchange->request.url.http_origin()->port,
                                                        target.addresses()[usize()])
                                               .as_str()
                                               .as_bytes()));
            if (target.unix_path().is_some())
                set(CURLOPT_UNIX_SOCKET_PATH, value.as_ptr());
            else {
                route = curl_slist_append(nullptr, value.as_ptr());
                if (! route) return Err(failure(CURLE_OUT_OF_MEMORY));
                set(CURLOPT_CONNECT_TO, route);
            }
        }
        if (code != CURLE_OK) return Err(failure(code));
        return Ok(empty {});
    }
    auto progress() -> lf::Result<empty> {
        if (! connected) return Ok(empty {});
        if (! eof && channel.available() > usize()) {
            array<u8, 16384> buffer {};
            size_t           count {};
            auto code = curl_easy_recv(easy,
                                       buffer.data(),
                                       rstd::min(usize(16384), channel.available()).to_primitive(),
                                       &count);
            if (code != CURLE_OK && code != CURLE_AGAIN) return Err(failure(code));
            if (code == CURLE_OK) {
                if (! count) {
                    eof = true;
                    channel.end_input();
                } else {
                    auto queued = channel.try_receive(Bytes::copy_from_slice(
                        slice<u8>::from_raw_parts(buffer.data(), usize(count))));
                    if (queued.is_err()) return Err(rstd::move(queued).unwrap_err());
                }
            }
        }
        if (writing.is_none()) {
            auto next = channel.try_write();
            if (next.is_err()) return Err(rstd::move(next).unwrap_err());
            writing = rstd::move(next).unwrap();
            offset  = usize();
        }
        if (writing.is_none()) return Ok(empty {});
        if (writing->is_half_close()) {
            if (exchange->request.url.scheme() == Some("https"_str))
                writing->complete(
                    Err(lf::Error::Unsupported("TLS send half-close is unavailable"_Str)));
            else {
                curl_socket_t socket = CURL_SOCKET_BAD;
                auto          code   = curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &socket);
                if (code != CURLE_OK) return Err(failure(code));
#if RSTD_OS_WINDOWS
                auto result = ::shutdown(socket, SD_SEND);
#else
                auto result = ::shutdown(socket, SHUT_WR);
#endif
                if (result != 0)
                    return Err(lf::Error::Connect(rstd::io::error::Error::last_os_error()));
                writing->complete(Ok(usize()));
            }
            writing = None();
            return Ok(empty {});
        }
        auto&  bytes = writing->bytes();
        size_t count {};
        auto   code = curl_easy_send(easy,
                                     bytes.as_slice().as_raw_ptr() + offset.to_primitive(),
                                     (bytes.len() - offset).to_primitive(),
                                     &count);
        if (code == CURLE_AGAIN) return Ok(empty {});
        if (code != CURLE_OK) return Err(failure(code));
        if (! count && ! bytes.is_empty()) return Err(failure(CURLE_SEND_ERROR));
        offset += usize(count);
        if (offset == bytes.len()) {
            writing->complete(Ok(offset));
            writing = None();
        }
        return Ok(empty {});
    }
};
struct Worker {
    Arc<BackendState>          state;
    rstd::async::RuntimeHandle runtime;
    void                       request_stop(lf::StopReason) const { stop(state); }
    static auto                perform(Arc<BackendState> state) -> lf::Result<empty> {
        auto* multi = curl_multi_init();
        if (! multi) {
            fail_pending(state, failure(CURLE_FAILED_INIT));
            return Err(failure(CURLE_FAILED_INIT));
        }
        Vec<Box<Active>>    active;
        Vec<Box<RawActive>> raw;
        Option<lf::Error>   fatal;
        for (;;) {
            Vec<Arc<Exchange>>    queued;
            Vec<Arc<RawExchange>> raw_queued;
            bool                  closed;
            {
                auto f     = state->fields.lock().unwrap();
                queued     = rstd::move(f->queued);
                raw_queued = rstd::move(f->raw);
                closed     = f->closed;
            }
            for (auto& exchange : queued) {
                if (closed) {
                    exchange->stop();
                    exchange->complete();
                    continue;
                }
                auto operation = Box<Active>::make(exchange.clone(), state->options.head_limit);
                auto ready     = operation->configure(state->options);
                auto added =
                    ready.is_ok() ? curl_multi_add_handle(multi, operation->easy) : CURLM_OK;
                if (ready.is_err() || added != CURLM_OK) {
                    exchange->fail(ready.is_err() ? rstd::move(ready).unwrap_err()
                                                  : failure(added));
                    operation->cleanup();
                    exchange->complete();
                    continue;
                }
                active.push(rstd::move(operation));
            }
            for (auto& exchange : raw_queued) {
                if (closed || exchange->stopped.is_canceled()) {
                    exchange->stop();
                    exchange->complete();
                    continue;
                }
                auto operation = Box<RawActive>::make(exchange.clone());
                auto ready     = operation->configure(state->options);
                auto added =
                    ready.is_ok() ? curl_multi_add_handle(multi, operation->easy) : CURLM_OK;
                if (ready.is_err() || added != CURLM_OK) {
                    exchange->fail(ready.is_err() ? rstd::move(ready).unwrap_err()
                                                  : failure(added));
                    operation->cleanup();
                    exchange->complete();
                    continue;
                }
                raw.push(rstd::move(operation));
            }
            for (usize i {}; i < raw.len();) {
                auto& operation = *raw[i];
                if (closed || operation.exchange->stopped.is_canceled() ||
                    operation.channel.cancellation().is_canceled()) {
                    auto removed = curl_multi_remove_handle(multi, operation.easy);
                    if (removed != CURLM_OK) {
                        fatal = Some(failure(removed));
                        break;
                    }
                    auto exchange = operation.exchange.clone();
                    if (closed) exchange->stop();
                    raw.remove(i);
                    exchange->complete();
                    continue;
                }
                auto progressed = operation.progress();
                if (progressed.is_err())
                    operation.exchange->fail(rstd::move(progressed).unwrap_err());
                ++i;
            }
            for (usize i {}; i < active.len();) {
                auto& operation = *active[i];
                bool  canceled;
                {
                    auto f   = operation.exchange->fields.lock().unwrap();
                    canceled = f->stopped || f->error.is_some();
                }
                if (closed || canceled) {
                    auto removed = curl_multi_remove_handle(multi, operation.easy);
                    if (removed != CURLM_OK) {
                        fatal = Some(failure(removed));
                        break;
                    }
                    auto exchange = operation.exchange.clone();
                    if (closed) exchange->stop();
                    active.remove(i);
                    exchange->complete();
                    continue;
                }
                if (operation.paused || operation.upload_paused) {
                    operation.paused = operation.upload_paused = false;
                    auto resumed = curl_easy_pause(operation.easy, CURLPAUSE_CONT);
                    if (resumed != CURLE_OK) operation.exchange->fail(failure(resumed));
                }
                ++i;
            }
            if (fatal.is_some()) break;
            int  running {};
            auto code = curl_multi_perform(multi, &running);
            if (code != CURLM_OK) {
                fatal = Some(failure(code));
                break;
            }
            int messages {};
            while (auto* message = curl_multi_info_read(multi, &messages)) {
                if (message->msg != CURLMSG_DONE) continue;
                for (auto& operation : raw) {
                    if (operation->easy != message->easy_handle) continue;
                    if (message->data.result != CURLE_OK)
                        operation->exchange->fail(failure(message->data.result));
                    else {
                        operation->connected                               = true;
                        operation->exchange->fields.lock().unwrap()->ready = true;
                        operation->exchange->changed.notify();
                    }
                    break;
                }
                for (usize i {}; i < active.len(); ++i)
                    if (active[i]->easy == message->easy_handle) {
                        auto& operation = *active[i];
                        auto  removed   = curl_multi_remove_handle(multi, operation.easy);
                        if (removed != CURLM_OK) {
                            fatal = Some(failure(removed));
                            break;
                        }
                        if (message->data.result != CURLE_OK)
                            operation.exchange->fail(failure(message->data.result));
                        auto exchange = operation.exchange.clone();
                        active.remove(i);
                        exchange->complete();
                        break;
                    }
            }
            if (fatal.is_some()) break;
            if (closed && active.is_empty() && raw.is_empty()) break;
            auto polled = curl_multi_poll(multi, nullptr, 0, 10, nullptr);
            if (polled != CURLM_OK) {
                fatal = Some(failure(polled));
                break;
            }
        }
        if (fatal.is_some()) {
            fail_pending(state, *fatal);
            for (const auto& operation : active) {
                operation->exchange->fail(fatal->clone());
                (void)curl_multi_remove_handle(multi, operation->easy);
            }
            for (const auto& operation : raw) {
                operation->exchange->fail(fatal->clone());
                (void)curl_multi_remove_handle(multi, operation->easy);
            }
        }
        auto cleaned = curl_multi_cleanup(multi);
        for (auto& operation : active) {
            operation->cleanup();
            operation->exchange->complete();
        }
        active.clear();
        for (auto& operation : raw) {
            operation->cleanup();
            operation->exchange->complete();
        }
        raw.clear();
        if (fatal.is_some()) return Err(fatal.take().unwrap());
        if (cleaned != CURLM_OK) return Err(failure(cleaned));
        return Ok(empty {});
    }
    auto run(lf::Cancellation) const -> coro<lf::Result<empty>> {
        auto job = runtime.spawn_blocking([state = state.clone()]() mutable {
            return perform(rstd::move(state));
        });
        if (job.is_err()) {
            auto error = lf::Error::Connect(job.unwrap_err());
            fail_pending(state, error);
            co_return Err(rstd::move(error));
        }
        auto joined = co_await rstd::move(job).unwrap();
        if (joined.is_err()) {
            fail_pending(state, lf::Error::Closed());
            co_return Err(lf::Error::Closed());
        }
        co_return rstd::move(joined).unwrap();
    }
};
struct Owner {
    Arc<BackendState> state;
    void              request_stop(lf::StopReason) const { stop(state); }
    auto              release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        stop(state);
        if (state->worker.is_some()) co_return co_await state->worker->wait();
        co_return Ok(empty {});
    }
};
struct RawDriver {
    Arc<BackendState> backend;
    Arc<RawExchange>  exchange;
    void              request_stop(lf::StopReason reason) const { exchange->stop(reason); }
    auto              release(lf::ReleaseContext) const -> coro<lf::Result<empty>> {
        exchange->stop();
        for (;;) {
            auto event = exchange->changed.listen();
            {
                auto f = exchange->fields.lock().unwrap();
                if (! f->dispatched || f->done) break;
            }
            co_await rstd::move(event);
        }
        auto f = exchange->fields.lock().unwrap();
        if (f->channel.is_some()) f->channel->release_buffers();
        f->permit = None();
        co_return Ok(empty {});
    }
    auto run(lf::TransferContext context, lf::OutputSender<lf::DuplexStream> output) const
        -> coro<lf::ProtocolCompletion> {
        auto attempt = context.begin_attempt();
        if (attempt.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(attempt).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        auto permit = co_await context.resources().reserve(
            lf::ResourceAmount { .resources = usize(1), .bytes = usize(16384) });
        if (permit.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(permit).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        exchange->fields.lock().unwrap()->permit = Some(rstd::move(permit).unwrap());
        auto channel =
            co_await lf::DuplexChannel::make(context.clone(),
                                             backend->options.window,
                                             exchange->request.url.scheme() != Some("https"_str));
        if (channel.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(channel).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        exchange->fields.lock().unwrap()->channel = Some(channel->clone());
        {
            auto f = backend->fields.lock().unwrap();
            if (f->closed)
                co_return lf::ProtocolCompletion { Err(lf::Error::Closed()),
                                                   lf::RemoteOutcome::NotDispatched };
            exchange->fields.lock().unwrap()->dispatched = true;
            f->raw.push(exchange.clone());
        }
        for (;;) {
            auto event = exchange->changed.listen();
            {
                auto f = exchange->fields.lock().unwrap();
                if (f->error.is_some())
                    co_return lf::ProtocolCompletion { Err(f->error->clone()),
                                                       lf::RemoteOutcome::Unknown };
                if (f->ready) break;
            }
            co_await rstd::move(event);
        }
        if (output.publish(channel->take_stream().unwrap()).is_err()) channel->request_stop();
        co_return lf::ProtocolCompletion { co_await channel->wait_finished(),
                                           lf::RemoteOutcome::Unknown };
    }
};
struct Driver {
    Arc<BackendState> backend;
    Arc<Exchange>     exchange;
    void              request_stop(lf::StopReason reason) const { exchange->stop(reason); }
    auto release(lf::ReleaseContext) const { return release_exchange(exchange.clone()); }
    auto run(lf::TransferContext context, lf::OutputSender<h::ResponseMessage> output) const
        -> coro<lf::ProtocolCompletion> {
        auto begun = co_await begin_exchange(exchange.clone(), context.clone());
        if (begun.is_err())
            co_return lf::ProtocolCompletion { Err(rstd::move(begun).unwrap_err()),
                                               lf::RemoteOutcome::NotDispatched };
        {
            auto f = backend->fields.lock().unwrap();
            if (f->closed)
                co_return lf::ProtocolCompletion { Err(lf::Error::Closed()),
                                                   lf::RemoteOutcome::NotDispatched };
            exchange->fields.lock().unwrap()->dispatched = true;
            f->queued.push(exchange.clone());
        }
        co_return co_await deliver(exchange.clone(), rstd::move(output));
    }
};
} // namespace ncrequest::client::curl
namespace rstd
{
template<>
struct Impl<lifetchto::ManagedResource, ncrequest::client::curl::Owner>
    : ImplBase<ncrequest::client::curl::Owner> {
    void request_stop(lifetchto::StopReason r) const { this->self().request_stop(r); }
    auto release(lifetchto::ReleaseContext c) const { return this->self().release(c); }
};
} // namespace rstd
namespace ncrequest::client::curl
{
Backend::Backend(Arc<BackendState> state, Arc<lf::ResourceLease<empty>> registration)
    : state_(rstd::move(state)), registration_(rstd::move(registration)) {}
Backend::Backend(Backend&&) noexcept = default;
void drop(const Arc<BackendState>& state) {
    if (! state) return;
    bool last;
    {
        auto f = state->fields.lock().unwrap();
        last   = --f->handles == usize();
    }
    if (last) stop(state);
}
Backend::~Backend() { drop(state_); }
auto Backend::operator=(Backend&& other) noexcept -> Backend& {
    if (this != &other) {
        drop(state_);
        state_        = rstd::move(other.state_);
        registration_ = rstd::move(other.registration_);
    }
    return *this;
}
auto Backend::clone() const -> Backend {
    {
        auto f = state_->fields.lock().unwrap();
        ++f->handles;
    }
    return Backend(state_.clone(), registration_.clone());
}
auto Backend::make(lf::ResourceContext resources, Options options) -> lf::Result<Backend> {
    if (options.window < usize(CURL_MAX_WRITE_SIZE) || options.window > usize(0x7fffffff) ||
        options.head_limit < usize(16) || options.head_limit > usize(0x7fffffff) ||
        options.connect_timeout.is_zero())
        return Err(lf::Error::InvalidOptions("invalid curl limits"_Str));
    if (options.tls.use_system_roots && ! options.tls.ca_pem.is_empty())
        return Err(lf::Error::Unsupported(
            "curl custom CA requires an explicit custom-only trust store"_Str));
    auto tcp = options.tcp.validate(curl_tcp_capabilities());
    if (tcp.is_err()) return Err(rstd::move(tcp).unwrap_err());
    if (options.low_speed.is_some()) {
        auto valid = options.low_speed->validate();
        if (valid.is_err()) return Err(rstd::move(valid).unwrap_err());
    }
    auto proxy = options.proxy.validate(proxy_capabilities());
    if (proxy.is_err()) return Err(rstd::move(proxy).unwrap_err());
    if (options.proxy_tls.use_system_roots && ! options.proxy_tls.ca_pem.is_empty())
        return Err(
            lf::Error::Unsupported("curl proxy custom CA requires a custom-only trust store"_Str));
    static auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return Err(failure(initialized));
    auto state    = Arc<BackendState>::make(rstd::move(options));
    auto attached = resources.attach(
        empty {}, Arc<rstd::dyn<lf::ManagedResource>>::make(Owner { state.clone() }));
    if (attached.is_err()) return Err(rstd::move(attached).unwrap_err().error);
    auto registration = Arc<lf::ResourceLease<empty>>::make(rstd::move(attached).unwrap());
    auto started      = resources.spawn(Worker { state.clone(), resources.runtime() });
    if (started.is_err()) return Err(rstd::move(started).unwrap_err().error);
    state->worker = Some(rstd::move(started).unwrap());
    return Ok(Backend(rstd::move(state), rstd::move(registration)));
}
auto Backend::backend() const -> Arc<rstd::dyn<h::ExchangeBackend>> {
    return Arc<rstd::dyn<h::ExchangeBackend>>::make(clone());
}
auto Backend::capabilities() const -> h::BackendCapabilities {
    auto* version = curl_version_info(CURLVERSION_NOW);
    return { .http1       = true,
             .https       = bool(version->features & CURL_VERSION_SSL),
             .unix_socket = bool(version->features & CURL_VERSION_UNIX_SOCKETS) &&
                            ! state_->options.proxy.enabled() && ! state_->options.tcp.specified(),
             .explicit_routes         = ! state_->options.proxy.enabled(),
             .unknown_upload_length   = true,
             .upload_trailers         = true,
             .response_trailers       = true,
             .repeated_request_fields = true,
             .response_version        = true,
             .expect_continue         = true,
             .duplex                  = state_->options.low_speed.is_none(),
             .half_close              = state_->options.low_speed.is_none(),
             .tcp                     = curl_tcp_capabilities(),
             .tls_custom_ca           = bool(version->features & CURL_VERSION_SSL),
             .tls_identity            = bool(version->features & CURL_VERSION_SSL),
             .tls_verification        = bool(version->features & CURL_VERSION_SSL),
             .low_speed               = true,
             .connect_timeout         = true,
             .proxy                   = proxy_capabilities() };
}
auto Backend::duplex(h::DuplexRequest input) const
    -> rstd::Result<lf::PreparedOperation<lf::DuplexStream>, lf::Rejected<h::DuplexRequest>> {
    auto              origin = input.url.http_origin();
    Option<lf::Error> error;
    if (state_->options.low_speed.is_some())
        error =
            Some(lf::Error::Unsupported("curl cannot measure low-speed policy for raw duplex"_Str));
    else if (state_->fields.lock().unwrap()->closed)
        error = Some(lf::Error::Closed());
    else if (origin.is_none() || input.url.userinfo().is_some())
        error = Some(lf::Error::InvalidOptions("HTTP origin required for duplex"_Str));
    else if (input.route.route.is_some()) {
        const auto& route = **input.route.route;
        if (state_->options.proxy.enabled())
            error = Some(
                lf::Error::Unsupported("explicit routes require a disabled proxy profile"_Str));
        else if (route.unix_path().is_some() && state_->options.tcp.specified())
            error =
                Some(lf::Error::Unsupported("TCP options cannot be applied to Unix sockets"_Str));
        else if (route.origin() != origin->serialize().as_str())
            error = Some(lf::Error::InvalidOptions("duplex route belongs to another origin"_Str));
        else if (route.unix_path().is_none() && route.addresses().len() != usize(1))
            error = Some(lf::Error::Unsupported("curl explicit routes require one address"_Str));
    }
    auto selected = state_->options.proxy.select(input.url);
    if (selected.is_err())
        error = Some(rstd::move(selected).unwrap_err());
    else if (selected->is_some() && input.route.route.is_some())
        error = Some(lf::Error::InvalidOptions("proxy conflicts with an explicit route"_Str));
    if (error.is_some())
        return Err(lf::Rejected<h::DuplexRequest> { error.take().unwrap(), rstd::move(input) });
    return Ok(lf::PreparedOperation<lf::DuplexStream>::make(
        RawDriver { state_.clone(),
                    Arc<RawExchange>::make(rstd::move(input), rstd::move(selected).unwrap()) }));
}
auto Backend::prepare(lihttpto::RequestParts parts, h::UploadMetadata upload,
                      h::RouteChoice route) const -> lf::Result<h::PreparedExchange> {
    if (state_->fields.lock().unwrap()->closed) return Err(lf::Error::Closed());
    if (route.route.is_some() && state_->options.proxy.enabled())
        return Err(lf::Error::Unsupported("explicit routes require a disabled proxy profile"_Str));
    if (parts.url.http_origin().is_none())
        return Err(lf::Error::InvalidOptions("HTTP origin required"_Str));
    if (route.route.is_some() &&
        (*route.route)->origin() != parts.url.http_origin()->serialize().as_str())
        return Err(lf::Error::InvalidOptions("route belongs to another origin"_Str));
    if (route.route.is_some() && (*route.route)->unix_path().is_none() &&
        (*route.route)->addresses().len() != usize(1))
        return Err(lf::Error::Unsupported("curl explicit routes require one address"_Str));
    if (route.route.is_some() && (*route.route)->unix_path().is_some() &&
        state_->options.tcp.specified())
        return Err(lf::Error::Unsupported("TCP options cannot be applied to Unix sockets"_Str));
    auto proxy = state_->options.proxy.select(parts.url);
    if (proxy.is_err()) return Err(rstd::move(proxy).unwrap_err());
    if (proxy->is_some() && route.route.is_some())
        return Err(lf::Error::InvalidOptions("proxy conflicts with an explicit route"_Str));
    auto prepared = prepare_exchange(state_->identity.clone(),
                                     rstd::move(parts),
                                     rstd::move(upload),
                                     rstd::move(route),
                                     state_->options.head_limit);
    if (prepared.is_ok()) prepared->set_proxy(rstd::move(proxy).unwrap());
    return prepared;
}
auto Backend::operation(h::ExchangeInput input) const
    -> rstd::Result<lf::PreparedOperation<h::ResponseMessage>, lf::Rejected<h::ExchangeInput>> {
    if (state_->fields.lock().unwrap()->closed)
        return Err(lf::Rejected<h::ExchangeInput> { lf::Error::Closed(), rstd::move(input) });
    if (! input.prepared.backend_identity().matches(state_->identity))
        return Err(lf::Rejected<h::ExchangeInput> {
            lf::Error::InvalidOptions("exchange belongs to another backend"_Str),
            rstd::move(input) });
    if (input.body.length() != input.prepared.upload().length)
        return Err(lf::Rejected<h::ExchangeInput> {
            lf::Error::Contract("upload metadata mismatch"_Str), rstd::move(input) });
    return Ok(lf::PreparedOperation<h::ResponseMessage>::make(
        Driver { state_.clone(),
                 Arc<Exchange>::make(
                     rstd::move(input), state_->options.window, state_->options.head_limit) }));
}
void Backend::request_close() const { stop(state_); }
auto Backend::close() const -> coro<lf::Result<empty>> {
    request_close();
    return registration_->release();
}
} // namespace ncrequest::client::curl
