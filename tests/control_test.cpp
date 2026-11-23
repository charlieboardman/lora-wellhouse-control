#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../firmware/gateway/receiver.c"
static gw_packet command(uint8_t kind, uint32_t id) {
    gw_packet p = {};
    p.kind = kind;
    p.session = 123;
    p.receiver_boot = 456;
    p.transaction = id;
    return p;
}
static void hello(gw_receiver &r, uint64_t now) {
    gw_packet p = command(GW_HELLO, 1), o;
    assert(gw_receiver_request(&r, &p, now * 1000, &o) == 1);
}
int main() {
    for (uint64_t base : {0ULL, 4294967290ULL, 631152000000ULL}) {
        gw_receiver r;
        gw_receiver_init(&r, 456);
        hello(r, base);
        gw_packet p = command(GW_ON, 2), o;
        assert(gw_receiver_request(&r, &p, base * 1000, &o) == 1);
        assert(r.on);
        gw_receiver_tick(&r, base + 129999);
        assert(r.on);
        gw_receiver_tick(&r, base + 130000);
        assert(!r.on);
        p = command(GW_ON, 3);
        gw_receiver_request(&r, &p, (base + 140000) * 1000, &o);
        assert(r.on);
        uint64_t deadline = r.relay_until_ms;
        gw_receiver_request(&r, &p, (base + 150000) * 1000, &o);
        assert(r.relay_until_ms == deadline);
        p = command(GW_PAUSE, 4);
        p.length = 8;
        gw_put64(p.data, PAUSE_MS);
        gw_receiver_request(&r, &p, (base + 160000) * 1000, &o);
        assert(!r.on);
        uint64_t expiry = r.pause_until_ms;
        gw_receiver_request(&r, &p, (base + 170000) * 1000, &o);
        assert(r.pause_until_ms == expiry);
        p = command(GW_ON, 5);
        gw_receiver_request(&r, &p, (base + 180000) * 1000, &o);
        assert(!r.on);
        gw_receiver_tick(&r, expiry);
        assert(!r.on);
        p = command(GW_ON, 6);
        gw_receiver_request(&r, &p, expiry * 1000, &o);
        assert(r.on);
        p = command(GW_PAUSE, 7);
        p.length = 8;
        gw_put64(p.data, PAUSE_MS);
        gw_receiver_request(&r, &p, (expiry + 1) * 1000, &o);
        p = command(GW_RESUME, 8);
        gw_receiver_request(&r, &p, (expiry + 2) * 1000, &o);
        assert(!r.on && !r.pause_until_ms);
        p = command(GW_ON, 6);
        gw_receiver_request(&r, &p, (expiry + 3) * 1000, &o);
        assert(!r.on);
        p = command(GW_ON, 9);
        gw_receiver_request(&r, &p, (expiry + 4) * 1000, &o);
        assert(r.on);
        gw_receiver_fail(&r);
        assert(!r.on);
        gw_receiver_init(&r, 789);
        assert(!r.on && !r.pause_until_ms && !r.clock.valid);
    }
    // A new sender session resets communication identity, not physical control.
    gw_receiver r;
    gw_receiver_init(&r, 456);
    hello(r, 0);
    gw_packet p = command(GW_ON, 2), o;
    gw_receiver_request(&r, &p, 1000, &o);
    const uint64_t relay_deadline = r.relay_until_ms;
    r.control_id = 3;
    r.modbus_id = 4;
    r.clock_id = 5;
    r.cache.valid = true;
    r.serial_pending = true;
    r.clock.valid = true;
    r.clock.offset_us = 123456;
    p = command(GW_HELLO, 1);
    p.session = 124;
    assert(gw_receiver_request(&r, &p, 2000, &o) == 1);
    assert(r.session == 124 && r.on && r.relay_until_ms == relay_deadline);
    assert(!r.control_id && !r.on_id && !r.modbus_id && !r.clock_id);
    assert(!r.cache.valid && !r.serial_pending);
    assert(r.clock.valid && r.clock.offset_us == 123456);

    p = command(GW_PAUSE, 2);
    p.session = 124;
    p.length = 8;
    gw_put64(p.data, PAUSE_MS);
    gw_receiver_request(&r, &p, 3000, &o);
    const uint64_t pause_deadline = r.pause_until_ms;
    p = command(GW_HELLO, 1);
    p.session = 125;
    gw_receiver_request(&r, &p, 4000, &o);
    assert(!r.on && r.pause_until_ms == pause_deadline);
    // A repeated HELLO in the same session must not erase duplicate tracking.
    r.control_id = 9;
    r.cache.valid = true;
    gw_receiver_request(&r, &p, 5000, &o);
    assert(r.control_id == 9 && r.cache.valid);
    gw_receiver_fail(&r);
    assert(!r.on && !r.relay_until_ms && r.pause_until_ms == pause_deadline);
    puts("PASS receiver timing, duplicate commands, resume, reboot, session boundaries, fail-off, "
         "large timestamps");
}
