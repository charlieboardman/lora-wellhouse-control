#include <cassert>
#include <cstdio>
#include <vector>
#include "../firmware/gateway/sender.c"
#include "../firmware/gateway/receiver.c"
static unsigned deliveries, controls;
static uint8_t last_error;
static bool confirmed;
static std::vector<uint8_t> delivered;
void gw_deliver(uint32_t, uint16_t, const uint8_t *p, size_t n, uint8_t error) {
    ++deliveries;
    last_error = error;
    delivered.assign(p, p + n);
}
void gw_control_done(uint32_t, bool ok) {
    ++controls;
    confirmed = ok;
}
static gw_sender s;
static gw_receiver r;
static uint64_t now;
static void exchange(gw_packet p, bool lose = false) {
    gw_packet o = {};
    int action = gw_receiver_request(&r, &p, now * 1000, &o);
    assert(action == 1);
    if (!lose) {
        assert(gw_sender_reply(&s, &o, now * 1000));
    } else {
        gw_sender_timeout(&s, now);
    }
}
static gw_packet next(void) {
    gw_packet p = {};
    assert(gw_sender_next(&s, now, &p));
    return p;
}
static void init(void) {
    now = 0;
    deliveries = controls = 0;
    last_error = 0;
    gw_sender_init(&s, 123, now);
    gw_receiver_init(&r, 456);
    exchange(next());
    s.sync_pending = false;
}
static void serial_response(gw_packet p, bool large, bool lose) {
    gw_packet o = {};
    assert(gw_receiver_request(&r, &p, now * 1000, &o) == 2);
    uint8_t frame[256] = {1, p.data[1]};
    size_t n = large ? 254 : 27;
    for (size_t i = 2; i < n; i++) {
        frame[i] = (uint8_t)i;
    }
    if (p.data[1] == 3) {
        frame[2] = 24;
    }
    uint16_t crc = gw_crc(frame, n);
    frame[n++] = (uint8_t)crc;
    frame[n++] = (uint8_t)(crc >> 8);
    gw_receiver_serial_done(&r, frame, n, now * 1000, &o);
    if (lose) {
        gw_sender_timeout(&s, now);
    } else {
        assert(gw_sender_reply(&s, &o, now * 1000));
    }
}
int main() {
    const uint8_t read[] = {1, 3, 0x21, 0, 0, 12}, vendor[] = {1, 0x41};
    gw_packet p, o;
    init();
    assert(!gw_sender_submit(&s, 1, 10, vendor, sizeof(vendor), now));
    p = next();
    assert(p.kind == GW_REQUEST);
    serial_response(p, true, false);
    assert(s.fetching);
    now = 30000;
    gw_sender_sample(&s, true, now);
    p = next();
    assert(p.kind == GW_ON);
    exchange(p);
    assert(r.on);
    p = next();
    assert(p.kind == GW_FETCH && p.piece == 0);
    exchange(p, true);
    p = next();
    assert(p.kind == GW_FETCH && p.piece == 0);
    exchange(p);
    p = next();
    assert(p.kind == GW_FETCH && p.piece == 1);
    exchange(p);
    assert(deliveries == 1 && !last_error && delivered.size() == 254 && !s.active);
    // Lost execution response reuses receiver cache, including after pump control.
    assert(!gw_sender_submit(&s, 1, 11, read, sizeof(read), now));
    p = next();
    serial_response(p, false, true);
    assert(gw_sender_control(&s, true, 99, now));
    o = next();
    assert(o.kind == GW_PAUSE);
    exchange(o);
    o = next();
    assert(o.kind == GW_REQUEST && o.transaction == p.transaction);
    exchange(o);
    assert(deliveries == 2 && r.cache.transaction == p.transaction && !r.on);
    // Pause acknowledgement loss cannot extend expiry, and resume requires fresh ON.
    init();
    now = 30000;
    gw_sender_sample(&s, true, now);
    exchange(next());
    assert(gw_sender_control(&s, true, 99, now));
    p = next();
    exchange(p, true);
    uint64_t expiry = r.pause_until_ms;
    now += 1000;
    p = next();
    exchange(p);
    assert(r.pause_until_ms == expiry && confirmed);
    assert(gw_sender_control(&s, false, 99, now));
    exchange(next());
    assert(!r.on);
    exchange(next());
    assert(r.on);
    // A pause lost before reception retries only the remaining duration.
    init();
    assert(gw_sender_control(&s, true, 99, now));
    p = next();
    gw_sender_timeout(&s, now);
    now = 1000;
    p = next();
    assert(gw_get64(p.data) == PAUSE_MS - now);
    exchange(p);
    assert(r.pause_until_ms == PAUSE_MS);
    // A receiver reboot cannot replay an uncertain write.
    init();
    assert(!gw_sender_submit(&s, 1, 12, read, sizeof(read), now));
    p = next();
    serial_response(p, false, true);
    gw_receiver_init(&r, 789);
    p = next();
    exchange(p);
    assert(deliveries == 1 && last_error == 11 && !s.active);
    assert(s.peer == 0);
    exchange(next());
    assert(s.peer == 789);
    // A failed priority ON may force a handshake while a write retry waits.
    // Remember the old receiver boot even after peer is cleared for reconnect.
    init();
    assert(!gw_sender_submit(&s, 1, 12, read, sizeof(read), now));
    p = next();
    serial_response(p, false, true);
    now = 30000;
    gw_sender_sample(&s, true, now);
    p = next();
    assert(p.kind == GW_ON);
    gw_sender_timeout(&s, now);
    assert(!s.peer && s.active);
    gw_receiver_init(&r, 789);
    exchange(next());
    assert(deliveries == 1 && last_error == 11 && !s.active && !s.retry_pending);
    // Invalid ACKs cannot report a confirmed control operation.
    init();
    assert(gw_sender_control(&s, true, 99, now));
    p = next();
    assert(gw_receiver_request(&r, &p, now * 1000, &o) == 1);
    o.length = 0;
    assert(!gw_sender_reply(&s, &o, now * 1000) && !controls);
    // Bad clock measurements back off and allow queued Modbus work through.
    init();
    gw_sender_ntp(&s, 0, UINT64_C(1800000000000000));
    p = next();
    gw_sender_sent(&s, 1000000);
    assert(gw_receiver_request(&r, &p, 1001000, &o) == 1);
    gw_put64(o.data + 8, 4000000);
    assert(gw_sender_reply(&s, &o, 9000000));
    now = 9000;
    assert(s.sync_pending && s.sync_retry_ms > now);
    assert(!gw_sender_submit(&s, 1, 12, read, sizeof(read), now));
    p = next();
    assert(p.kind == GW_REQUEST);
    // Bounded queue, single-packet request cap, and two timeouts release the job.
    init();
    uint8_t large[GW_DATA_MAX + 1] = {1, 3};
    assert(gw_sender_submit(&s, 1, 1, large, sizeof(large), now) == 3);
    for (unsigned i = 0; i < GW_QUEUE_SIZE; i++) {
        assert(!gw_sender_submit(&s, 1, (uint16_t)i, read, sizeof(read), now));
    }
    assert(gw_sender_submit(&s, 1, 1, read, sizeof(read), now) == 6);
    p = next();
    gw_sender_timeout(&s, now);
    p = next();
    gw_sender_timeout(&s, now);
    assert(deliveries == 1 && last_error == 11);
    // Retry handshake, then reject stale responses without releasing the active slot.
    init();
    s.peer = 0;
    p = next();
    gw_sender_timeout(&s, now);
    o = next();
    assert(o.transaction == p.transaction);
    exchange(o);
    assert(!gw_sender_submit(&s, 1, 3, read, sizeof(read), now));
    p = next();
    o = p;
    o.kind = GW_RESULT;
    ++o.transaction;
    assert(!gw_sender_reply(&s, &o, now * 1000) && s.have_out);
    // Clock exchange uses TX/RX end timestamps, excluding airtime and scheduling.
    init();
    uint64_t utc = UINT64_C(1800000000000000);
    gw_sender_ntp(&s, 0, utc);
    p = next();
    assert(p.kind == GW_SYNC);
    gw_sender_sent(&s, 1000000);
    assert(gw_receiver_request(&r, &p, 1001000, &o) == 1);
    gw_put64(o.data + 8, 4000000);
    assert(gw_sender_reply(&s, &o, 4001000));
    assert(s.correction_pending);
    now = 5000;
    p = next();
    assert(p.kind == GW_CORRECTION);
    exchange(p);
    assert(r.clock.valid && r.clock.offset_us == (int64_t)utc);
    // Sample-open cancellation while a request waits; no catch-up ON burst.
    init();
    now = 30000;
    gw_sender_sample(&s, true, now);
    assert(s.on_pending);
    gw_sender_sample(&s, false, now + 1000);
    assert(!s.on_pending);
    assert(!gw_sender_next(&s, now + 1000, &p));
    puts("PASS sender/receiver execution: priority ON, cached writes, piece retries, session "
         "resets, queue bounds, time sync, stale replies");
}
