#if ! defined(LITO_FEAT_QT) && defined(__linux__)
#    include <rstd/test/gtest.hpp>
#    include <poll.h>
#    include <sys/socket.h>
#    include <cstdlib>
import ncrequest;
import lihttpto;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace curl;
using enum curl::CURLcode;
using enum curl::CURLoption;
using enum curl::CURLINFO;

struct CurlUpgradeProbe {
    CURL*         handle { curl_easy_init() };
    CURLM*        multi {};
    bool          added {};
    curl_socket_t socket { -1 };
    ~CurlUpgradeProbe() {
        if (added) (void)curl_multi_remove_handle(multi, handle);
        if (handle) curl_easy_cleanup(handle);
        if (multi) curl_multi_cleanup(multi);
    }

    auto connect(const char* path, bool use_multi) -> bool {
        if (! handle) return false;
        if (curl_easy_setopt(handle, CURLOPT_URL, "http://localhost/") != CURLE_OK ||
            curl_easy_setopt(handle, CURLOPT_UNIX_SOCKET_PATH, path) != CURLE_OK ||
            curl_easy_setopt(handle, CURLOPT_PROXY, "") != CURLE_OK ||
            curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK ||
            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
            curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 2000L) != CURLE_OK)
            return false;
        if (use_multi) {
            multi = curl_multi_init();
            if (! multi || curl_multi_add_handle(multi, handle) != CURLMcode::CURLM_OK)
                return false;
            added          = true;
            bool connected = false;
            for (unsigned attempt = 0; attempt < 100 && ! connected; ++attempt) {
                int running {};
                if (curl_multi_perform(multi, &running) != CURLMcode::CURLM_OK) return false;
                int remaining {};
                while (auto* message = curl_multi_info_read(multi, &remaining)) {
                    if (message->msg != CURLMSG::CURLMSG_DONE || message->easy_handle != handle ||
                        message->data.result != CURLE_OK)
                        return false;
                    connected = true;
                }
                if (! connected &&
                    curl_multi_poll(multi, nullptr, 0, 50, nullptr) != CURLMcode::CURLM_OK)
                    return false;
            }
            if (! connected) return false;
        } else if (curl_easy_perform(handle) != CURLE_OK)
            return false;
        return curl_easy_getinfo(handle, CURLINFO_ACTIVESOCKET, &socket) == CURLE_OK && socket >= 0;
    }
    auto wait(short events) -> bool {
        pollfd fd { socket, events, 0 };
        return ::poll(&fd, 1, 2000) == 1 && ! (fd.revents & POLLNVAL);
    }
    auto send(slice<u8> input) -> bool {
        usize offset;
        while (offset < input.len()) {
            rstd::size_t written {};
            auto         code = curl_easy_send(handle,
                                               input.as_raw_ptr() + offset.to_primitive(),
                                               (input.len() - offset).to_primitive(),
                                               &written);
            if (code == CURLE_AGAIN) {
                if (! wait(POLLOUT)) return false;
                continue;
            }
            if (code != CURLE_OK || ! written) return false;
            offset += usize(written);
        }
        return true;
    }
    auto receive() -> Option<rstd::bytes::Bytes> {
        array<u8, 4096> bytes {};
        for (;;) {
            rstd::size_t received {};
            auto         code = curl_easy_recv(handle, bytes.data(), 4096, &received);
            if (code == CURLE_AGAIN) {
                if (! wait(POLLIN)) return None();
                continue;
            }
            if (code != CURLE_OK) return None();
            return Some(rstd::bytes::Bytes::copy_from_slice(
                slice<u8>::from_raw_parts(bytes.data(), usize(received))));
        }
    }
};

TEST(CurlUpgradeProbe, UnixHalfClosePreservesOutput) {
    ASSERT_TRUE(ncrequest::curl_init().is_ok());
    const auto* path = ::getenv("NCREQUEST_UPGRADE_SOCKET");
    ASSERT_TRUE(path != nullptr);
    for (unsigned mode = 0; mode < 4; ++mode) {
        bool             fragmented = (mode & 1) != 0;
        CurlUpgradeProbe wire;
        ASSERT_TRUE(wire.connect(path, (mode & 2) != 0));
        auto request =
            fragmented
                ? "POST /fragmented HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: tcp\r\nContent-Length: 0\r\n\r\n"_str
                : "POST /coalesced HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: tcp\r\nContent-Length: 0\r\n\r\n"_str;
        ASSERT_TRUE(wire.send(request.as_bytes()));
        lihttpto::Http1HeadParser parser;
        String                    output;
        bool                      upgraded = false;
        while (! upgraded) {
            auto bytes = wire.receive();
            ASSERT_TRUE(bytes.is_some() && ! bytes->is_empty());
            auto event = parser.push(bytes->as_slice());
            ASSERT_TRUE(event.is_ok());
            if (event->is_NeedMore()) continue;
            auto complete = rstd::move(event).unwrap().as_Complete();
            ASSERT_TRUE(complete.head.status_code() == Some(u16(101)));
            for (usize i = complete.input_consumed; i < bytes->len(); ++i)
                output.push_ascii(bytes->as_slice()[i]);
            upgraded = true;
        }
        ASSERT_TRUE(wire.send("input"_str.as_bytes()));
        ASSERT_TRUE(::shutdown(wire.socket, SHUT_WR) == 0);
        for (;;) {
            auto bytes = wire.receive();
            ASSERT_TRUE(bytes.is_some());
            if (bytes->is_empty()) break;
            for (auto byte : bytes->as_slice()) output.push_ascii(u8(byte));
        }
        EXPECT_TRUE(output.as_str() == "first:tail-after-eof"_str);
    }
}
#endif
