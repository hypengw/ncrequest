#include <rstd/test/gtest.hpp>
#include <stdlib.h>
#if LITO_FEAT_QT
#    include <QThread>
#    include <QNetworkAccessManager>
#    include <QNetworkProxy>
#    include <QCoreApplication>
#    include <mutex>
#    include <condition_variable>
#    include <thread>
#    include <atomic>
import ncrequest;
#    include "ws_helpers.hpp"
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace qt = client::qt_network;
using lf::Duration;
auto qt_ws_env(const char* name) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(::getenv(name)).to_str().unwrap();
}
auto qt_ws_target(ref<str> path) -> DuplexRequest {
    return { lihttpto::Url::parse_http(
                 rstd::format("{}{}", qt_ws_env("NCREQUEST_QT_WS_URL"), path).as_str())
                 .unwrap(),
             {} };
}
auto qt_ws_open(Arc<Session> session, ref<str> path, WebSocketOptions options = {}) {
    return WebSocketConnection::connect(rstd::move(session), qt_ws_target(path), {}, {}, options);
}
TEST(QtWebSocket, MessageStreamLimitsAndCloseOwnTheirNativeExit) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = qt::Backend::make(engine.resources()).unwrap();
        auto session = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        EXPECT_FALSE(session->capabilities().duplex);
        EXPECT_TRUE(session->websocket_capabilities().supported);
        EXPECT_FALSE(session->websocket_capabilities().handshake_response);
        EXPECT_FALSE(session->websocket_capabilities().peer_close_confirmation);
        EXPECT_FALSE(session->websocket_capabilities().receive_backpressure);
        EXPECT_FALSE(session->websocket_capabilities().receive_limit_details);
        EXPECT_TRUE(session->websocket_capabilities().frame_sized_read_window);
        auto connection = (co_await qt_ws_open(session.clone(), "/stream"_str)).unwrap();
        EXPECT_TRUE(connection.protocol().is_none());
        {
            auto messages = connection.messages().unwrap();
            EXPECT_EQ(connection.messages().unwrap_err(), WebSocketError::Busy);
            EXPECT_EQ((co_await read_ws(connection)).unwrap_err(), WebSocketError::Busy);
            auto timed = co_await rstd::async::timeout(rstd::stream::next(messages),
                                                       Duration::from_millis(u64(1)));
            EXPECT_TRUE(timed.is_err());
            auto first = (co_await rstd::stream::next(messages)).unwrap();
            EXPECT_TRUE(first.is_ok() && first->data.is_empty());
            auto second = (co_await rstd::stream::next(messages)).unwrap().unwrap();
            EXPECT_EQ(second.kind, liwebsocketo::Kind::Binary);
            EXPECT_TRUE(second.data.as_slice() == "binary"_str.as_bytes());
            EXPECT_TRUE((co_await rstd::stream::next(messages)).is_none());
        }
        EXPECT_TRUE((co_await connection.shutdown()).is_ok());
        EXPECT_TRUE((co_await connection.completion().wait())->cleanup.is_ok());
        auto echo    = (co_await qt_ws_open(session.clone(), "/echo"_str)).unwrap();
        auto pending = echo.receive();
        EXPECT_EQ(echo.messages().unwrap_err(), WebSocketError::Busy);
        pending.close();
        EXPECT_TRUE((co_await echo.shutdown()).is_ok());
        EXPECT_TRUE((co_await echo.completion().wait())->logical.result.is_err());
        for (int mode {}; mode < 2; ++mode) {
            WebSocketOptions limits;
            if (mode)
                limits.limits.message_bytes = usize(2);
            else
                limits.limits.frame_bytes = usize(2);
            auto bounded  = (co_await qt_ws_open(session.clone(), "/stream"_str, limits)).unwrap();
            auto received = (co_await read_ws(bounded));
            if (received.is_ok()) {
                EXPECT_TRUE(received->data.is_empty());
                received = (co_await read_ws(bounded));
            }
            EXPECT_TRUE(received.is_err());
            if (received.is_err()) EXPECT_EQ(received.unwrap_err(), WebSocketError::IncomingLimit);
            EXPECT_TRUE((co_await bounded.completion().wait())->cleanup.is_ok());
            EXPECT_TRUE((co_await bounded.shutdown()).is_ok());
        }
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(QtWebSocket, HandshakeCancellationDeadlinesAndUnsupportedOptions) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = qt::Backend::make(engine.resources()).unwrap();
        auto session = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        for (int scenario {}; scenario < 2; ++scenario) {
            WebSocketOptions options;
            options.upgrade.handshake_timeout = Duration::from_millis(u64(scenario ? 500 : 25));
            if (scenario)
                options.upgrade.duplex.total_timeout =
                    Some(lf::http::TotalTimeout::after(Duration::from_millis(u64(25))));
            auto opened = co_await qt_ws_open(session.clone(), "/stall"_str, options);
            EXPECT_TRUE(opened.is_err());
            if (opened.is_err()) {
                auto e = rstd::move(opened).unwrap_err();
                EXPECT_TRUE(e.transport.is_some() && e.transport->is_Timeout());
                EXPECT_TRUE(e.report.is_some() && (*e.report)->cleanup.is_ok());
            }
        }
        {
            auto opening = qt_ws_open(session.clone(), "/stall"_str).into_future();
            auto race    = co_await rstd::async::select(
                rstd::future::by_ref(opening), rstd::async::sleep(Duration::from_millis(u64(20))));
            EXPECT_TRUE(race.is_right());
        }
        WebSocketOptions limited;
        limited.upgrade.header_bytes = usize(32);
        auto unsupported = co_await qt_ws_open(session.clone(), "/must-not-dispatch"_str, limited);
        EXPECT_TRUE(unsupported.is_err());
        if (unsupported.is_err())
            EXPECT_TRUE(unsupported.as_ref().unwrap_err().admission->is_Unsupported());
        lihttpto::Headers duplicate;
        duplicate.add("X-Dup"_str, "a"_str).unwrap();
        duplicate.add("X-Dup"_str, "b"_str).unwrap();
        auto rejected = co_await WebSocketConnection::connect(
            session.clone(), qt_ws_target("/must-not-dispatch"_str), rstd::move(duplicate));
        EXPECT_TRUE(rejected.is_err() &&
                    rejected.as_ref().unwrap_err().admission->is_Unsupported());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(QtWebSocket, SharedSessionBackendCloseAndRepeatedShutdown) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = qt::Backend::make(engine.resources()).unwrap();
        auto first   = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        auto second  = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        auto a       = (co_await qt_ws_open(first.clone(), "/echo"_str)).unwrap();
        auto b       = (co_await qt_ws_open(second.clone(), "/echo"_str)).unwrap();
        auto waiting = a.receive();
        EXPECT_TRUE((co_await first->close()).is_ok());
        EXPECT_TRUE((co_await waiting.message()).is_err());
        EXPECT_TRUE(
            (co_await write_ws(
                 b, { liwebsocketo::Kind::Text, Bytes::copy_from_slice("shared"_str.as_bytes()) }))
                .is_ok());
        EXPECT_TRUE((co_await read_ws(b)).unwrap().data.as_slice() == "shared"_str.as_bytes());
        auto pending = b.receive();
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_TRUE((co_await pending.message()).is_err());
        (void)co_await rstd::async::timeout(b.shutdown().into_future(), Duration {});
        EXPECT_TRUE((co_await b.shutdown()).is_ok());
        EXPECT_TRUE((co_await b.shutdown()).is_ok());
        EXPECT_TRUE(
            (co_await b.completion().wait())->logical.result.as_ref().unwrap_err().is_Canceled());
        EXPECT_TRUE((co_await a.shutdown()).is_ok());
        EXPECT_TRUE((co_await second->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(QtWebSocket, WssUsesTheImmutableQtTlsProfileAndTypedError) {
    if (! ::getenv("NCREQUEST_QT_WS_TLS_URL")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine = lf::Engine::make(runtime.handle()).unwrap();
        for (int scenario {}; scenario < 2; ++scenario) {
            qt::Options options;
            if (! scenario) {
                options.tls.use_system_roots = false;
                auto pem                     = rstd::fs::read(
                                                   rstd::path::PathBuf::from(qt_ws_env("NCREQUEST_QT_WS_CA")).as_path())
                                                   .unwrap();
                options.tls.ca_pem.push(Bytes::copy_from_slice(pem.as_slice()));
            }
            auto backend = qt::Backend::make(engine.resources(), rstd::move(options)).unwrap();
            auto session =
                Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
            auto opened = co_await WebSocketConnection::connect(
                session.clone(),
                { lihttpto::Url::parse_http(qt_ws_env("NCREQUEST_QT_WS_TLS_URL")).unwrap(), {} });
            if (! scenario) {
                EXPECT_TRUE(opened.is_ok());
                if (opened.is_ok()) {
                    auto c = rstd::move(opened).unwrap();
                    EXPECT_TRUE((co_await write_ws(c, { liwebsocketo::Kind::Binary, {} })).is_ok());
                    EXPECT_TRUE((co_await read_ws(c)).unwrap().data.is_empty());
                    EXPECT_TRUE((co_await c.close()).is_ok());
                    EXPECT_TRUE((co_await c.shutdown()).is_ok());
                }
            } else {
                EXPECT_TRUE(opened.is_err());
                if (opened.is_err()) {
                    auto e = rstd::move(opened).unwrap_err();
                    EXPECT_TRUE(e.transport.is_some() && e.transport->is_Extension());
                    if (e.transport.is_some() && e.transport->is_Extension())
                        EXPECT_TRUE(rstd::error::downcast_ref<qt::WebSocketNativeError>(
                                        e.transport->as_Extension().cause.deref())
                                        .is_some());
                    EXPECT_TRUE(e.report.is_some() && (*e.report)->cleanup.is_ok());
                }
            }
            EXPECT_TRUE((co_await session->close()).is_ok());
            EXPECT_TRUE((co_await backend.close()).is_ok());
        }
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(QtWebSocket, ExternalManagerDestructionDrainsWebSocketsOnItsThread) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto    runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    QNetworkAccessManager* manager {};
    Option<qt::Backend>    backend;
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            manager = new QNetworkAccessManager;
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            qt::Options options;
            options.manager = manager;
            backend = Some(qt::Backend::make(engine.resources(), rstd::move(options)).unwrap());
        },
        Qt::BlockingQueuedConnection);
    auto test = [&]() -> coro<empty> {
        auto session = Session::from_backend(engine.handle(), backend->shared_binding()).unwrap();
        auto closed_session =
            Session::from_backend(engine.handle(), backend->shared_binding()).unwrap();
        auto closed = (co_await qt_ws_open(closed_session.clone(), "/echo"_str)).unwrap();
        EXPECT_TRUE((co_await closed_session->close()).is_ok());
        EXPECT_TRUE((co_await closed.shutdown()).is_ok());
        auto http =
            session
                ->get(Request::from_url(
                          rstd::format("{}/http", qt_ws_env("NCREQUEST_QT_WS_URL")).as_str())
                          .unwrap())
                .unwrap();
        EXPECT_EQ((co_await (co_await http.response()).unwrap()->text()).unwrap().as_str(),
                  "http"_str);
        auto first = (co_await qt_ws_open(session.clone(), "/echo"_str)).unwrap();
        EXPECT_TRUE((co_await write_ws(first,
                                       { liwebsocketo::Kind::Text,
                                         Bytes::copy_from_slice("manager"_str.as_bytes()) }))
                        .is_ok());
        EXPECT_TRUE((co_await read_ws(first)).unwrap().data.as_slice() == "manager"_str.as_bytes());
        auto pending = first.receive();
        QMetaObject::invokeMethod(
            &bridge,
            [&] {
                delete manager;
                manager = nullptr;
            },
            Qt::BlockingQueuedConnection);
        EXPECT_TRUE((co_await pending.message()).is_err());
        EXPECT_TRUE((co_await first.shutdown()).is_ok());
        EXPECT_TRUE((co_await first.completion().wait())->cleanup.is_ok());
        EXPECT_TRUE((co_await first.completion().wait())
                        ->logical.result.as_ref()
                        .unwrap_err()
                        .is_Canceled());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend->close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
}
TEST(QtWebSocket, ProtocolValidationAndDroppedStreamStopTheOwner) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = qt::Backend::make(engine.resources()).unwrap();
        auto session = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        lihttpto::Headers headers;
        headers.add("Host"_str, "other.invalid"_str).unwrap();
        auto rejected = co_await WebSocketConnection::connect(
            session.clone(), qt_ws_target("/must-not-dispatch"_str), rstd::move(headers));
        EXPECT_TRUE(rejected.is_err() && rejected.as_ref().unwrap_err().handshake.is_some());
        WebSocketOptions options;
        options.limits.message_bytes = usize(2);
        auto connection = (co_await qt_ws_open(session.clone(), "/echo"_str, options)).unwrap();
        EXPECT_EQ((co_await write_ws(connection, { liwebsocketo::Kind::Pong, {} })).unwrap_err(),
                  WebSocketError::Protocol);
        EXPECT_EQ((co_await write_ws(
                       connection,
                       { liwebsocketo::Kind::Text, Bytes::copy_from_slice("long"_str.as_bytes()) }))
                      .unwrap_err(),
                  WebSocketError::MessageTooLarge);
        auto sent = co_await write_ws(
            connection,
            { liwebsocketo::Kind::Ping, Bytes::copy_from_slice("ping"_str.as_bytes()) });
        EXPECT_TRUE(sent.is_ok());
        auto pong = (co_await read_ws(connection)).unwrap();
        EXPECT_EQ(pong.kind, liwebsocketo::Kind::Pong);
        EXPECT_TRUE(pong.data.as_slice() == "ping"_str.as_bytes());
        {
            auto stream   = connection.messages().unwrap();
            auto canceled = co_await rstd::async::timeout(rstd::stream::next(stream),
                                                          Duration::from_millis(u64(1)));
            EXPECT_TRUE(canceled.is_err());
        }
        auto report = co_await connection.completion().wait();
        EXPECT_TRUE(report->logical.result.as_ref().unwrap_err().is_Canceled());
        EXPECT_TRUE((co_await connection.shutdown()).is_ok());
        auto abrupt   = (co_await qt_ws_open(session.clone(), "/abrupt"_str)).unwrap();
        auto received = (co_await read_ws(abrupt));
        EXPECT_TRUE(received.is_err());
        if (received.is_err()) EXPECT_EQ(received.unwrap_err(), WebSocketError::Transport);
        auto failure = co_await abrupt.completion().wait();
        EXPECT_TRUE(failure->logical.result.is_err());
        EXPECT_TRUE((co_await abrupt.shutdown()).is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
struct ExecutorProbe {
    rstd::sync::Mutex<Vec<int>> sequence { Vec<int> {} };
    lf::Event                   changed;
    std::mutex                  gate;
    std::condition_variable     progress;
    bool                        allow {}, started {};
};
TEST(QtWebSocket, CallbackExecutorThreadOrderAndActiveShutdownBarrier) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto    runtime = rstd::async::RuntimeBuilder::multi_thread()
                          .worker_threads(usize(3))
                          .enable_all()
                          .build()
                          .unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    Option<qt::CallbackExecutor> executor;
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            executor = Some(qt::CallbackExecutor::make(&bridge).unwrap());
        },
        Qt::BlockingQueuedConnection);
    auto test = [&]() -> coro<empty> {
        auto backend = qt::Backend::make(engine.resources()).unwrap();
        auto session = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        for (int mode {}; mode < 3; ++mode) {
            auto               probe = Arc<ExecutorProbe>::make();
            WebSocketCallbacks callbacks;
            callbacks.executor  = Some(rstd::async::AnyExecutor::from_executor(executor->clone()));
            callbacks.connected = [probe = probe.clone(), &thread] {
                EXPECT_EQ(QThread::currentThread(), &thread);
                probe->sequence.lock().unwrap()->push(0);
                probe->changed.notify();
            };
            callbacks.message = [probe = probe.clone(), &thread, mode](liwebsocketo::Message) {
                EXPECT_EQ(QThread::currentThread(), &thread);
                probe->sequence.lock().unwrap()->push(1);
                if (mode == 1) {
                    std::unique_lock lock(probe->gate);
                    probe->started = true;
                    probe->changed.notify();
                    probe->progress.wait(lock, [&] {
                        return probe->allow;
                    });
                }
                probe->changed.notify();
            };
            callbacks.error = [probe = probe.clone(), &thread](WebSocketError) {
                EXPECT_EQ(QThread::currentThread(), &thread);
                probe->sequence.lock().unwrap()->push(2);
                probe->changed.notify();
            };
            callbacks.disconnected = [probe = probe.clone(), &thread] {
                EXPECT_EQ(QThread::currentThread(), &thread);
                probe->sequence.lock().unwrap()->push(3);
                probe->changed.notify();
            };
            auto client = (co_await WebSocketClient::connect(
                               session.clone(),
                               qt_ws_target(mode == 2 ? "/abrupt"_str : "/echo"_str),
                               rstd::move(callbacks)))
                              .unwrap();
            if (mode == 2) {
                for (;;) {
                    auto event = probe->changed.listen();
                    if (probe->sequence.lock().unwrap()->len() == usize(3)) break;
                    co_await rstd::move(event);
                }
                EXPECT_TRUE((co_await client.shutdown()).is_ok());
                auto values = probe->sequence.lock().unwrap();
                EXPECT_EQ((*values)[usize()], 0);
                EXPECT_EQ((*values)[usize(1)], 2);
                EXPECT_EQ((*values)[usize(2)], 3);
                continue;
            }
            EXPECT_TRUE((co_await write_ws(client,
                                           { liwebsocketo::Kind::Text,
                                             Bytes::copy_from_slice("executor"_str.as_bytes()) }))
                            .is_ok());
            for (;;) {
                auto event = probe->changed.listen();
                bool ready;
                if (mode == 1) {
                    std::lock_guard lock(probe->gate);
                    ready = probe->started;
                } else
                    ready = probe->sequence.lock().unwrap()->len() == usize(2);
                if (ready) break;
                co_await rstd::move(event);
            }
            if (mode) {
                auto interrupted = co_await rstd::async::timeout(client.shutdown().into_future(),
                                                                 Duration::from_millis(u64(2)));
                EXPECT_TRUE(interrupted.is_err());
                {
                    std::lock_guard lock(probe->gate);
                    probe->allow = true;
                }
                probe->progress.notify_all();
            } else {
                EXPECT_TRUE(client.request_close().is_ok());
                EXPECT_TRUE((co_await client.close()).is_ok());
                for (;;) {
                    auto event = probe->changed.listen();
                    if (probe->sequence.lock().unwrap()->len() == usize(3)) break;
                    co_await rstd::move(event);
                }
            }
            EXPECT_TRUE((co_await client.shutdown()).is_ok());
            auto values = probe->sequence.lock().unwrap();
            EXPECT_EQ(values->len(), usize(mode ? 2 : 3));
            EXPECT_EQ((*values)[usize()], 0);
            EXPECT_EQ((*values)[usize(1)], 1);
            if (! mode) EXPECT_EQ((*values)[usize(2)], 3);
        }
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            executor->close();
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
}
TEST(QtWebSocket, DestroyedCallbackContextCancelsAcceptedJobs) {
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    Option<qt::CallbackExecutor> executor;
    int                          ran {}, canceled {};
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            auto* context = new QObject;
            executor      = Some(qt::CallbackExecutor::make(context).unwrap());
            EXPECT_TRUE(executor->post_job(rstd::async::ExecutorJob::make(
                [&] {
                    ++ran;
                },
                [&] {
                    ++canceled;
                })));
            delete context;
        },
        Qt::BlockingQueuedConnection);
    EXPECT_TRUE(executor->is_closed());
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(canceled, 1);
    EXPECT_FALSE(executor->post_job(rstd::async::ExecutorJob::make(
        [&] {
            ++ran;
        },
        [&] {
            ++canceled;
        })));
    EXPECT_EQ(canceled, 2);
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
}
TEST(QtWebSocket, OwnedSessionClosesTheSharedBackend) {
    if (! ::getenv("NCREQUEST_QT_WS_URL")) GTEST_SKIP();
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto test    = [&]() -> coro<empty> {
        auto engine  = lf::Engine::make(runtime.handle()).unwrap();
        auto backend = qt::Backend::make(engine.resources()).unwrap();
        auto owner   = Session::from_backend(engine.handle(), backend.owned_binding()).unwrap();
        auto sibling = Session::from_backend(engine.handle(), backend.shared_binding()).unwrap();
        auto wire    = (co_await qt_ws_open(sibling.clone(), "/echo"_str)).unwrap();
        auto pending = wire.receive();
        EXPECT_TRUE((co_await owner->close()).is_ok());
        EXPECT_TRUE((co_await pending.message()).is_err());
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_TRUE((co_await sibling->close()).is_ok());
        EXPECT_TRUE((co_await backend.close()).is_ok());
        EXPECT_EQ(engine.resources().usage().bytes, usize());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
}
TEST(QtWebSocket, CallbackPostingCanRaceContextDestruction) {
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    QObject*                     context {};
    Option<qt::CallbackExecutor> executor;
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            context  = new QObject;
            executor = Some(qt::CallbackExecutor::make(context).unwrap());
        },
        Qt::BlockingQueuedConnection);
    std::atomic<unsigned> ran {}, canceled {}, posted {};
    std::thread           producer([executor = executor->clone(), &ran, &canceled, &posted] {
        for (unsigned i {}; i < 2000; ++i) {
            (void)executor.post_job(rstd::async::ExecutorJob::make(
                [&ran] {
                    ++ran;
                },
                [&canceled] {
                    ++canceled;
                }));
            ++posted;
        }
    });
    while (posted.load() == 0) std::this_thread::yield();
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            delete context;
        },
        Qt::BlockingQueuedConnection);
    producer.join();
    EXPECT_TRUE(executor->is_closed());
    EXPECT_EQ(ran.load() + canceled.load(), 2000u);
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
}
#endif
