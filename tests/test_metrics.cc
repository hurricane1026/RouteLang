// Per-shard metrics tests: counters, histograms, aggregation, callback integration.
#include "rut/runtime/metrics.h"
#include "test.h"
#include "test_helpers.h"

using namespace rut;

// === LatencyHistogram: bucket selection ===

TEST(histogram, bucket_0_under_100us) {
    CHECK_EQ(LatencyHistogram::find_bucket(0), 0u);
    CHECK_EQ(LatencyHistogram::find_bucket(50), 0u);
    CHECK_EQ(LatencyHistogram::find_bucket(99), 0u);
}

TEST(histogram, bucket_1_100_to_500us) {
    CHECK_EQ(LatencyHistogram::find_bucket(100), 1u);
    CHECK_EQ(LatencyHistogram::find_bucket(499), 1u);
}

TEST(histogram, bucket_2_500_to_1ms) {
    CHECK_EQ(LatencyHistogram::find_bucket(500), 2u);
    CHECK_EQ(LatencyHistogram::find_bucket(999), 2u);
}

TEST(histogram, bucket_3_1ms_to_5ms) {
    CHECK_EQ(LatencyHistogram::find_bucket(1000), 3u);
    CHECK_EQ(LatencyHistogram::find_bucket(4999), 3u);
}

TEST(histogram, bucket_4_5ms_to_10ms) {
    CHECK_EQ(LatencyHistogram::find_bucket(5000), 4u);
    CHECK_EQ(LatencyHistogram::find_bucket(9999), 4u);
}

TEST(histogram, bucket_5_to_9_high_latency) {
    CHECK_EQ(LatencyHistogram::find_bucket(10000), 5u);
    CHECK_EQ(LatencyHistogram::find_bucket(50000), 6u);
    CHECK_EQ(LatencyHistogram::find_bucket(100000), 7u);
    CHECK_EQ(LatencyHistogram::find_bucket(500000), 8u);
    CHECK_EQ(LatencyHistogram::find_bucket(1000000), 9u);
}

TEST(histogram, bucket_10_overflow) {
    CHECK_EQ(LatencyHistogram::find_bucket(5000000), 10u);
    CHECK_EQ(LatencyHistogram::find_bucket(0xFFFFFFFF), 10u);
}

// === LatencyHistogram: recording ===

TEST(histogram, record_single) {
    LatencyHistogram h;
    h.init();
    h.record(150);  // bucket 1 (100-500μs)
    CHECK_EQ(h.buckets[1], 1u);
    CHECK_EQ(h.sum_us, 150u);
    CHECK_EQ(h.count, 1u);
}

TEST(histogram, record_multiple_buckets) {
    LatencyHistogram h;
    h.init();
    h.record(50);     // bucket 0
    h.record(200);    // bucket 1
    h.record(1500);   // bucket 3
    h.record(50000);  // bucket 6

    CHECK_EQ(h.buckets[0], 1u);
    CHECK_EQ(h.buckets[1], 1u);
    CHECK_EQ(h.buckets[3], 1u);
    CHECK_EQ(h.buckets[6], 1u);
    CHECK_EQ(h.count, 4u);
    CHECK_EQ(h.sum_us, static_cast<u64>(50 + 200 + 1500 + 50000));
}

TEST(histogram, record_same_bucket_accumulates) {
    LatencyHistogram h;
    h.init();
    for (u32 i = 0; i < 100; i++) h.record(50);
    CHECK_EQ(h.buckets[0], 100u);
    CHECK_EQ(h.count, 100u);
    CHECK_EQ(h.sum_us, 5000u);
}

// === ShardMetrics: basic operations ===

TEST(shard_metrics, init_zeros) {
    ShardMetrics m;
    m.init();
    CHECK_EQ(m.requests_total, 0u);
    CHECK_EQ(m.requests_active, 0u);
    CHECK_EQ(m.connections_total, 0u);
    CHECK_EQ(m.connections_active, 0u);
    CHECK_EQ(m.connections_closed, 0u);
}

TEST(shard_metrics, on_accept) {
    ShardMetrics m;
    m.init();
    m.on_accept();
    CHECK_EQ(m.connections_total, 1u);
    CHECK_EQ(m.connections_active, 1u);
    m.on_accept();
    CHECK_EQ(m.connections_total, 2u);
    CHECK_EQ(m.connections_active, 2u);
}

TEST(shard_metrics, on_close) {
    ShardMetrics m;
    m.init();
    m.on_accept();
    m.on_accept();
    m.on_close();
    CHECK_EQ(m.connections_active, 1u);
    CHECK_EQ(m.connections_closed, 1u);
}

TEST(shard_metrics, on_close_floor_at_zero) {
    ShardMetrics m;
    m.init();
    m.on_close();  // should not underflow
    CHECK_EQ(m.connections_active, 0u);
    CHECK_EQ(m.connections_closed, 1u);
}

TEST(shard_metrics, request_lifecycle) {
    ShardMetrics m;
    m.init();
    m.on_request_start();
    CHECK_EQ(m.requests_active, 1u);
    m.on_request_complete(250);
    CHECK_EQ(m.requests_active, 0u);
    CHECK_EQ(m.requests_total, 1u);
    CHECK_EQ(m.request_latency.count, 1u);
    CHECK_EQ(m.request_latency.sum_us, 250u);
    CHECK_EQ(m.request_latency.buckets[1], 1u);  // 250μs → bucket 1 (100-499μs)
}

// === Aggregation ===

TEST(aggregate, two_shards) {
    ShardMetrics s1, s2;
    s1.init();
    s2.init();

    s1.on_accept();
    s1.on_accept();
    s1.on_request_start();
    s1.on_request_complete(100);  // bucket 1

    s2.on_accept();
    s2.on_request_start();
    s2.on_request_complete(2000);  // bucket 3

    ShardMetrics* ptrs[] = {&s1, &s2};
    auto agg = aggregate_metrics(ptrs, 2);

    CHECK_EQ(agg.connections_total, 3u);
    CHECK_EQ(agg.connections_active, 3u);
    CHECK_EQ(agg.requests_total, 2u);
    CHECK_EQ(agg.request_latency.count, 2u);
    CHECK_EQ(agg.request_latency.sum_us, 2100u);
    CHECK_EQ(agg.request_latency.buckets[1], 1u);
    CHECK_EQ(agg.request_latency.buckets[3], 1u);
}

TEST(aggregate, empty) {
    auto agg = aggregate_metrics(nullptr, 0);
    CHECK_EQ(agg.requests_total, 0u);
    CHECK_EQ(agg.connections_total, 0u);
}

// === Callback integration ===

TEST(callback_metrics, records_on_response) {
    SmallLoop loop;
    loop.setup();

    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    // Accept → recv → send
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));

    CHECK_EQ(m.requests_total, 1u);
    CHECK_EQ(m.request_latency.count, 1u);
    CHECK(m.request_latency.sum_us < 1000000u);  // < 1 second
}

TEST(callback_metrics, no_crash_without_metrics) {
    SmallLoop loop;
    loop.setup();
    // metrics = nullptr (default)

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));
    CHECK(true);  // no crash
}

TEST(callback_metrics, requests_active_decremented_on_send_error) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    // Accept → recv (starts request) → send error (close_conn should decrement)
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    CHECK_EQ(m.requests_active, 1u);

    // Send error: ev.result < 0
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -1));
    CHECK_EQ(m.requests_active, 0u);  // decremented by close_conn_impl
}

TEST(callback_metrics, requests_active_decremented_on_partial_send) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    CHECK_EQ(m.requests_active, 1u);

    // Partial send: result != send_buf.len()
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, 1));
    CHECK_EQ(m.requests_active, 0u);
}

TEST(callback_metrics, requests_active_decremented_on_wrong_event) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    CHECK_EQ(m.requests_active, 1u);

    // Wrong event type during Send state
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    CHECK_EQ(m.requests_active, 0u);
}

// === Proxy callback tests ===

// Helper: set up a connection in proxy state, ready for on_upstream_connected
static void setup_proxy_conn(SmallLoop& loop, Connection*& c, u32& cid) {
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    c = loop.find_fd(42);
    cid = c->id;
    // Simulate recv to start request timing
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    // Now manually set up for proxy: wire to upstream_connected callback
    c->upstream_fd = 99;
    c->on_complete = &on_upstream_connected<SmallLoop>;
}

TEST(proxy_callback, upstream_connect_success) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);

    // Upstream connect succeeds → should forward request to upstream
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    CHECK_EQ(c->state, ConnState::Proxying);
    // Should have submitted a send to upstream
    CHECK(loop.backend.count_ops(MockOp::Send) > 0);
}

TEST(proxy_callback, upstream_connect_fail_502) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);

    // Upstream connect fails → should send 502
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, -1));
    CHECK_EQ(c->resp_status, static_cast<u16>(502));
    CHECK(!c->keep_alive);
}

TEST(proxy_callback, upstream_connect_wrong_event) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);

    // Wrong event type → close
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 0));
    CHECK_EQ(loop.conns[cid].fd, -1);
    CHECK_EQ(m.requests_active, 0u);
}

TEST(proxy_callback, upstream_request_sent_success) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    // Now at on_upstream_request_sent. Simulate full send of recv_buf.
    u32 req_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(req_len)));
    // Should now be waiting for upstream response (recv on upstream)
    CHECK(loop.backend.count_ops(MockOp::Recv) > 0);
}

TEST(proxy_callback, upstream_request_sent_error) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    // Send error
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

TEST(proxy_callback, upstream_request_sent_any_positive_succeeds) {
    // Backends guarantee full sends. Any positive result is success.
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, 1));
    // Should proceed to upstream response phase, not close.
    CHECK(loop.conns[cid].fd >= 0);
}

TEST(proxy_callback, upstream_request_sent_wrong_event) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    // Wrong event type
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// Helper: advance proxy conn to the upstream-response stage
static void advance_to_upstream_response(SmallLoop& loop, Connection*& c, u32& cid) {
    setup_proxy_conn(loop, c, cid);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    u32 req_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(req_len)));
    // recv_buf was reset by on_upstream_request_sent, ready for upstream response
}

TEST(proxy_callback, upstream_response_success) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);

    // Simulate upstream response data in recv_buf
    inject_upstream_response(loop, *c);
    CHECK_EQ(c->state, ConnState::Sending);
}

TEST(proxy_callback, upstream_response_error) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);

    // Upstream recv error
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, -1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

TEST(proxy_callback, upstream_response_wrong_event) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);

    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, 1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

TEST(proxy_callback, proxy_response_sent_success) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);
    inject_upstream_response(loop, *c);
    // Now at on_proxy_response_sent, send the proxied response to client
    u32 resp_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(resp_len)));
    // Should have completed the request cycle
    CHECK_EQ(m.requests_total, 1u);
    CHECK_EQ(c->state, ConnState::ReadingHeader);
}

TEST(proxy_callback, proxy_response_sent_error) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);
    inject_upstream_response(loop, *c);
    // Send error
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

TEST(proxy_callback, proxy_response_sent_any_positive_succeeds) {
    // Backends guarantee full sends. Any positive result is success.
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);
    inject_upstream_response(loop, *c);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, 1));
    // Should complete the request, not close on "partial".
    CHECK_EQ(loop.conns[cid].state, ConnState::ReadingHeader);
}

TEST(proxy_callback, proxy_response_sent_wrong_event) {
    SmallLoop loop;
    loop.setup();

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);
    inject_upstream_response(loop, *c);
    // Wrong event type
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

TEST(proxy_callback, proxy_response_sent_draining_closes) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    Connection* c = nullptr;
    u32 cid = 0;
    advance_to_upstream_response(loop, c, cid);
    inject_upstream_response(loop, *c);
    // During drain with no Connection header, on_upstream_response rebuilds
    // the response in send_buf with "Connection: close" injected, and routes
    // through on_response_sent. Use send_buf.len() for the send result.
    u32 resp_len = c->send_buf.len();
    CHECK_GT(resp_len, 0u);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(resp_len)));
    // During drain, connections should be closed after send
    CHECK_EQ(loop.conns[cid].fd, -1);
    CHECK_EQ(m.requests_total, 1u);
}

TEST(proxy_callback, 502_response_records_correct_status) {
    SmallLoop loop;
    loop.setup();
    ShardMetrics m;
    m.init();
    loop.metrics = &m;
    AccessLogRing ring;
    ring.init();
    loop.access_log = &ring;

    Connection* c = nullptr;
    u32 cid = 0;
    setup_proxy_conn(loop, c, cid);
    // Upstream connect fails → 502
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, -1));
    // Complete the 502 send
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(send_len)));

    CHECK_EQ(m.requests_total, 1u);
    // Access log should have status 502
    AccessLogEntry entry{};
    CHECK(ring.pop(entry));
    CHECK_EQ(entry.status, static_cast<u16>(502));
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
