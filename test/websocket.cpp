#include <rstd/test/gtest.hpp>
#include <stdlib.h>
import ncrequest;
#include "ws_helpers.hpp"
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
struct CallbackCounts {
    usize connected, disconnected, messages, errors;
    Bytes payload;
};
struct CallbackProbe {
    rstd::sync::Mutex<CallbackCounts> counts { CallbackCounts {} };
    lf::Event                         changed;
};
TEST(websocket, LocalEchoText) {
    if (! ::getenv("NCREQUEST_TEST_WS_URL")) GTEST_SKIP();
    for (int mode {}; mode < 3; ++mode) {
        if ((mode == 1 && ! LITO_FEAT_CURL) || (mode == 2 && ! LITO_FEAT_QT)) continue;
        auto runtime = rstd::async::RuntimeBuilder::multi_thread()
                           .worker_threads(usize(3))
                           .enable_all()
                           .build()
                           .unwrap();
        auto test    = [&]() -> coro<empty> {
            auto engine  = lf::Engine::make(runtime.handle()).unwrap();
            auto session = [&]() -> Arc<Session> {
#if LITO_FEAT_QT
                if (mode == 2)
                    return Session::from_backend(
                               engine.handle(),
                               client::qt_network::Backend::make(engine.resources())
                                   .unwrap()
                                   .owned_binding())
                        .unwrap();
#endif
                auto backend = [&]() -> Arc<rstd::dyn<lf::http::ExchangeBackend>> {
#if LITO_FEAT_CURL
                    if (mode == 1)
                        return client::curl::Backend::make(engine.resources()).unwrap().backend();
#endif
                    return lf::http::NativeBackend::make(engine.resources()).unwrap().backend();
                }();
                return Session::from_http_backend(
                           engine.handle(), lf::http::BackendBinding::Owned(rstd::move(backend)))
                    .unwrap();
            }();
            auto               probe = Arc<CallbackProbe>::make();
            WebSocketCallbacks callbacks;
            callbacks.connected = [probe = probe.clone()] {
                ++probe->counts.lock().unwrap()->connected;
                probe->changed.notify();
            };
            callbacks.disconnected = [probe = probe.clone()] {
                ++probe->counts.lock().unwrap()->disconnected;
                probe->changed.notify();
            };
            callbacks.message = [probe = probe.clone()](liwebsocketo::Message message) {
                {
                    auto f = probe->counts.lock().unwrap();
                    ++f->messages;
                    f->payload = rstd::move(message.data);
                }
                probe->changed.notify();
            };
            callbacks.error = [probe = probe.clone()](WebSocketError) {
                ++probe->counts.lock().unwrap()->errors;
                probe->changed.notify();
            };
            auto url =
                lihttpto::Url::parse_http(
                    rstd::ffi::CStr::from_ptr(::getenv("NCREQUEST_TEST_WS_URL")).to_str().unwrap())
                    .unwrap();
            auto connected = co_await WebSocketClient::connect(
                session.clone(), { rstd::move(url), {} }, rstd::move(callbacks));
            EXPECT_TRUE(connected.is_ok());
            if (connected.is_ok()) {
                auto client = rstd::move(connected).unwrap();
                EXPECT_TRUE(client.is_connected());
                auto sent =
                    co_await write_ws(client,
                                      { liwebsocketo::Kind::Text,
                                        Bytes::copy_from_slice("callback echo"_str.as_bytes()) });
                EXPECT_TRUE(sent.is_ok());
                for (;;) {
                    auto event = probe->changed.listen();
                    if (probe->counts.lock().unwrap()->messages == usize(1)) break;
                    co_await rstd::move(event);
                }
                EXPECT_TRUE(probe->counts.lock().unwrap()->payload.as_slice() ==
                            "callback echo"_str.as_bytes());
                auto closed = co_await client.close();
                EXPECT_TRUE(closed.is_ok());
                for (;;) {
                    auto event = probe->changed.listen();
                    if (probe->counts.lock().unwrap()->disconnected == usize(1)) break;
                    co_await rstd::move(event);
                }
                EXPECT_FALSE(client.is_connected());
                (void)co_await rstd::async::timeout(client.shutdown().into_future(),
                                                    rstd::time::Duration {});
                EXPECT_TRUE((co_await client.shutdown()).is_ok());
                EXPECT_TRUE((co_await client.shutdown()).is_ok());
                auto f = probe->counts.lock().unwrap();
                EXPECT_EQ(f->connected, usize(1));
                EXPECT_EQ(f->errors, usize());
            }
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_EQ(engine.resources().usage().bytes, usize());
            EXPECT_TRUE((co_await engine.close()).is_ok());
            co_return empty {};
        };
        runtime.block_on(test());
    }
}
