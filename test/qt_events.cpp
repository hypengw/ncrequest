#include <rstd/test/gtest.hpp>
#include <cstdlib>
#include <cstdio>
#if LITO_FEAT_QT
#    include <QThread>
#    include <QAbstractEventDispatcher>
#    include <QNetworkAccessManager>
#    include <QNetworkProxy>
#    include <QCoreApplication>
#    include <chrono>
#    include <ctime>
#    include <algorithm>
#    include <vector>
import ncrequest;
#    include "ws_helpers.hpp"
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
namespace lf = lifetchto;
namespace qt_bench
{
struct Samples {
    std::chrono::steady_clock::time_point enqueued;
    std::vector<double>                   started;
};
struct Probe {
    rstd::sync::Mutex<Samples> fields { Samples {} };
};
struct Endpoint {
    Arc<rstd::dyn<WebSocketEndpoint>> inner;
    Arc<Probe>                        probe;
    auto                              protocol() const { return inner->protocol(); }
    auto                              send(liwebsocketo::Message message) const {
        {
            auto f = probe->fields.lock().unwrap();
            f->started.push_back(std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - f->enqueued)
                                     .count());
        }
        return inner->send(rstd::move(message));
    }
    auto receive() const { return inner->receive(); }
    auto close(Bytes payload) const { return inner->close(rstd::move(payload)); }
    void cancel() const { inner->cancel(); }
    auto completion() const { return inner->completion(); }
    auto shutdown() const { return inner->shutdown(); }
    auto has_receiver() const { return inner->has_receiver(); }
    void release_received() const { inner->release_received(); }
    auto failure() const { return inner->failure(); }
    void discard_received() const { inner->discard_received(); }
    auto has_messages() const { return inner->has_messages(); }
};
struct Backend {
    Arc<rstd::dyn<WebSocketBackend>> inner;
    Arc<Probe>                       probe;
    auto                             capabilities() const { return inner->capabilities(); }
    auto connect(lf::ResourceContext, lf::http::ConnectionPlan, lihttpto::Headers, Vec<String>,
                 WebSocketOptions, WebSocketBackendEvents) const
        -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>>;
};
} // namespace qt_bench
namespace rstd
{
template<>
struct Impl<ncrequest::WebSocketEndpoint, qt_bench::Endpoint> : ImplBase<qt_bench::Endpoint> {
    auto protocol() const { return this->self().protocol(); }
    auto send(liwebsocketo::Message message) const {
        return this->self().send(rstd::move(message));
    }
    auto receive() const { return this->self().receive(); }
    auto close(ncrequest::Bytes payload) const { return this->self().close(rstd::move(payload)); }
    void cancel() const { this->self().cancel(); }
    auto completion() const { return this->self().completion(); }
    auto shutdown() const { return this->self().shutdown(); }
    auto has_receiver() const { return this->self().has_receiver(); }
    void release_received() const { this->self().release_received(); }
    auto failure() const { return this->self().failure(); }
    void discard_received() const { this->self().discard_received(); }
    auto has_messages() const { return this->self().has_messages(); }
};
template<>
struct Impl<ncrequest::WebSocketBackend, qt_bench::Backend> : ImplBase<qt_bench::Backend> {
    auto capabilities() const { return this->self().capabilities(); }
    auto connect(lifetchto::ResourceContext resources, lifetchto::http::ConnectionPlan plan,
                 lihttpto::Headers headers, ::alloc::vec::Vec<::alloc::string::String> protocols,
                 ncrequest::WebSocketOptions       options,
                 ncrequest::WebSocketBackendEvents events) const {
        return this->self().connect(rstd::move(resources),
                                    rstd::move(plan),
                                    rstd::move(headers),
                                    rstd::move(protocols),
                                    options,
                                    rstd::move(events));
    }
};
} // namespace rstd
auto qt_bench::Backend::connect(lf::ResourceContext resources, lf::http::ConnectionPlan plan,
                                lihttpto::Headers headers, Vec<String> protocols,
                                WebSocketOptions options, WebSocketBackendEvents events) const
    -> coro<rstd::Result<Arc<rstd::dyn<WebSocketEndpoint>>, WebSocketConnectFailure>> {
    auto opened = co_await inner->connect(rstd::move(resources),
                                          rstd::move(plan),
                                          rstd::move(headers),
                                          rstd::move(protocols),
                                          options,
                                          rstd::move(events));
    if (opened.is_err()) co_return Err(rstd::move(opened).unwrap_err());
    co_return Ok(Arc<rstd::dyn<WebSocketEndpoint>>::make(
        Endpoint { rstd::move(opened).unwrap(), probe.clone() }));
}
TEST(QtEvents, MeasureIdleAndSequentialMessages) {
    const char* output = std::getenv("NCREQUEST_WS_BENCHMARK_FILE");
    const char* target = std::getenv("NCREQUEST_QT_WS_URL");
    if (! output || ! target) GTEST_SKIP();
    const bool trace_writer = std::getenv("NCREQUEST_WS_BENCHMARK_TRACE_WRITER");
    auto    runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto    engine  = lf::Engine::make(runtime.handle()).unwrap();
    QThread thread;
    thread.start();
    QObject bridge;
    bridge.moveToThread(&thread);
    QNetworkAccessManager*              manager {};
    Option<client::qt_network::Backend> backend;
    unsigned                            wakes {}, idle_backend {}, idle_connection {};
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            manager = new QNetworkAccessManager;
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            auto* dispatcher = QAbstractEventDispatcher::instance(QThread::currentThread());
            QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, &bridge, [&] {
                ++wakes;
            });
            client::qt_network::Options options;
            options.manager = manager;
            backend =
                Some(client::qt_network::Backend::make(engine.resources(), rstd::move(options))
                         .unwrap());
        },
        Qt::BlockingQueuedConnection);
    auto                probe = Arc<qt_bench::Probe>::make();
    std::vector<double> latency, write_latency, writer_start;
    double              mixed_ms {};
    double              idle_cpu_ms {};
    auto                test = [&]() -> coro<empty> {
        auto binding =
            trace_writer
                ? BackendBinding::Shared(
                      backend->backend(),
                      Some(Arc<rstd::dyn<WebSocketBackend>>::make(qt_bench::Backend {
                          backend->shared_binding().websocket().unwrap(), probe.clone() })))
                : backend->shared_binding();
        auto session = Session::from_backend(engine.handle(), binding).unwrap();
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(50)));
        QMetaObject::invokeMethod(
            &bridge,
            [&] {
                wakes = 0;
            },
            Qt::BlockingQueuedConnection);
        auto cpu = std::clock();
        co_await rstd::async::sleep(lf::Duration::from_secs(u64(2)));
        idle_cpu_ms = double(std::clock() - cpu) * 1000.0 / CLOCKS_PER_SEC;
        QMetaObject::invokeMethod(
            &bridge,
            [&] {
                idle_backend = wakes;
            },
            Qt::BlockingQueuedConnection);
        auto address = rstd::format("{}/echo", rstd::ffi::CStr::from_ptr(target).to_str().unwrap());
        WebSocketOptions options;
        options.receive_policy = ReceivePolicy::Fail;
        auto wire              = (co_await WebSocketConnection::connect(
                                      session.clone(),
                                      { lihttpto::Url::parse_http(address.as_str()).unwrap(), {} },
                                      {},
                                      {},
                                      options))
                                     .unwrap();
        co_await rstd::async::sleep(lf::Duration::from_millis(u64(50)));
        QMetaObject::invokeMethod(
            &bridge,
            [&] {
                wakes = 0;
            },
            Qt::BlockingQueuedConnection);
        co_await rstd::async::sleep(lf::Duration::from_secs(u64(2)));
        QMetaObject::invokeMethod(
            &bridge,
            [&] {
                idle_connection = wakes;
            },
            Qt::BlockingQueuedConnection);
        for (int i {}; i < 100; ++i) {
            auto start = std::chrono::steady_clock::now();
            if (trace_writer) probe->fields.lock().unwrap()->enqueued = start;
            auto sent = co_await write_ws(
                wire,
                { liwebsocketo::Kind::Binary, Bytes::copy_from_slice("benchmark"_str.as_bytes()) });
            EXPECT_TRUE(sent.is_ok());
            write_latency.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
            auto received = co_await read_ws(wire);
            EXPECT_TRUE(received.is_ok());
            latency.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        Vec<WebSocketConnection> others;
        for (int i {}; i < 3; ++i)
            others.push((co_await WebSocketConnection::connect(
                             session.clone(),
                             { lihttpto::Url::parse_http(address.as_str()).unwrap(), {} },
                             {},
                             {},
                             options))
                            .unwrap());
        auto             mixed_start = std::chrono::steady_clock::now();
        Vec<PendingSend> queued;
        Vec<u8>          payload;
        payload.resize(usize(65536), u8('x'));
        for (usize i {}; i < usize(4); ++i) {
            auto& connection = i == usize() ? wire : others[i - usize(1)];
            for (int j {}; j < 8; ++j)
                queued.push(connection
                                .send({ liwebsocketo::Kind::Binary,
                                        Bytes::copy_from_slice(payload.as_slice()) })
                                .unwrap());
        }
        auto http = session
                        ->get(Request::from_url(
                                  rstd::format("{}/http",
                                               rstd::ffi::CStr::from_ptr(target).to_str().unwrap())
                                      .as_str())
                                  .unwrap())
                        .unwrap();
        for (auto& pending : queued) EXPECT_TRUE((co_await pending.written()).is_ok());
        for (usize i {}; i < usize(4); ++i) {
            auto& connection = i == usize() ? wire : others[i - usize(1)];
            for (int j {}; j < 8; ++j)
                EXPECT_EQ((co_await read_ws(connection)).unwrap().data.len(), usize(65536));
        }
        EXPECT_EQ((co_await (co_await http.response()).unwrap()->text()).unwrap().as_str(),
                  "http"_str);
        mixed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             mixed_start)
                       .count();
        for (auto& connection : others) {
            EXPECT_TRUE((co_await connection.close()).is_ok());
            EXPECT_TRUE((co_await connection.shutdown()).is_ok());
        }
        EXPECT_TRUE((co_await wire.close()).is_ok());
        EXPECT_TRUE((co_await wire.shutdown()).is_ok());
        EXPECT_TRUE((co_await session->close()).is_ok());
        EXPECT_TRUE((co_await backend->close()).is_ok());
        EXPECT_TRUE((co_await engine.close()).is_ok());
        co_return empty {};
    };
    runtime.block_on(test());
    QMetaObject::invokeMethod(
        &bridge,
        [&] {
            delete manager;
            bridge.moveToThread(QCoreApplication::instance()->thread());
        },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
    if (trace_writer) {
        auto f = probe->fields.lock().unwrap();
        ASSERT_GE(f->started.size(), 100u);
        writer_start.assign(f->started.begin(), f->started.begin() + 100);
    }
    std::sort(writer_start.begin(), writer_start.end());
    std::sort(latency.begin(), latency.end());
    std::sort(write_latency.begin(), write_latency.end());
    char writer_quantiles[128];
    if (trace_writer)
        std::snprintf(writer_quantiles,
                      sizeof(writer_quantiles),
                      "{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}",
                      writer_start[49],
                      writer_start[94],
                      writer_start[98]);
    auto* file = std::fopen(output, "w");
    ASSERT_TRUE(file);
    std::fprintf(file,
                 "{\"idle_backend_wakes_2s\":%u,\"idle_connection_wakes_2s\":%u,\"idle_cpu_ms\":%."
                 "3f,\"roundtrip_ms\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},\"samples\":[",
                 idle_backend,
                 idle_connection,
                 idle_cpu_ms,
                 latency[49],
                 latency[94],
                 latency[98]);
    for (size_t i {}; i < latency.size(); ++i)
        std::fprintf(file, "%s%.6f", i ? "," : "", latency[i]);
    std::fprintf(
        file,
        "] "
        ",\"write_observed_ms\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},\"mixed_four_connections_"
        "http_ms\":%.3f,\"mixed_admitted\":32,\"mixed_rejected\":0,\"initial_queue_per_connection_"
        "messages\":8,\"initial_queue_per_connection_bytes\":524288,\"writer_start_ms\":%s,"
        "\"writer_start_samples\":[",
        write_latency[49],
        write_latency[94],
        write_latency[98],
        mixed_ms,
        trace_writer ? writer_quantiles : "null");
    for (size_t i {}; i < writer_start.size(); ++i)
        std::fprintf(file, "%s%.6f", i ? "," : "", writer_start[i]);
    std::fprintf(file, "]}\n");
    std::fclose(file);
}
#endif
