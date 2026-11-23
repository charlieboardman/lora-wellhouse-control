#include "receiver.h"
static void reply(gw_receiver *r, const gw_packet *p, gw_packet *o, uint8_t kind) {
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    o->session = p->session;
    o->receiver_boot = r->boot;
    o->transaction = p->transaction;
    o->piece = p->piece;
}
static int error(gw_receiver *r, const gw_packet *p, gw_packet *o, uint8_t code) {
    reply(r, p, o, GW_ERROR);
    o->data[0] = code;
    o->length = 1;
    return 1;
}
static void result(gw_receiver *r, const gw_packet *p, gw_packet *o, uint64_t now_us) {
    if (r->cache_error) {
        error(r, p, o, r->cache_error);
        return;
    }
    size_t n = r->cache.response_length;
    reply(r, p, o, n + 16 <= GW_DATA_MAX ? GW_RESULT : GW_READY);
    o->data[0] = (uint8_t)(n >> 8);
    o->data[1] = (uint8_t)n;
    gw_put64(o->data + 2, r->cache.acquired_us);
    o->data[10] = (r->cache.acquired_us != 0);
    uint64_t age = r->clock.valid ? now_us / 1000 - r->clock.synced_ms : UINT32_MAX;
    if (age > UINT32_MAX) {
        age = UINT32_MAX;
    }
    for (unsigned i = 0; i < 4; i++) {
        o->data[11 + i] = (uint8_t)(age >> (24 - 8 * i));
    }
    o->data[15] = (uint8_t)((n + GW_DATA_MAX - 1) / GW_DATA_MAX);
    o->length = 16;
    if (o->kind == GW_RESULT) {
        memcpy(o->data + 16, r->cache.response, n);
        o->length += n;
    }
}
/* Session identity is separate from pump state: reconnecting must not cancel a
 * pause, extend a relay hold, or discard the receiver's UTC offset. */
static void begin_session(gw_receiver *r, uint64_t session) {
    r->session = session;
    r->control_id = 0;
    r->on_id = 0;
    r->modbus_id = 0;
    r->clock_id = 0;
    r->cache.valid = false;
    r->serial_pending = false;
}

void gw_receiver_init(gw_receiver *r, uint64_t boot) {
    memset(r, 0, sizeof(*r));
    r->boot = boot;
}
void gw_receiver_tick(gw_receiver *r, uint64_t now) {
    if (r->on && (now >= r->relay_until_ms || now < r->pause_until_ms)) {
        r->on = false;
    }
}
void gw_receiver_fail(gw_receiver *r) {
    r->on = false;
    r->relay_until_ms = 0;
}
int gw_receiver_request(gw_receiver *r, const gw_packet *p, uint64_t rx_us, gw_packet *o) {
    uint64_t now = rx_us / 1000;
    r->reply_deadline_us = rx_us + REPLY_START_MS * 1000;
    if (p->kind == GW_HELLO && p->session && p->length == 0) {
        if (r->session != p->session) {
            begin_session(r, p->session);
        }
        reply(r, p, o, GW_HELLO);
        return 1;
    }
    if (p->session != r->session || !p->session || p->receiver_boot != r->boot) {
        return error(r, p, o, 10);
    }
    if (p->kind == GW_ON && p->length == 0) {
        /* Only a fresh ON newer than the last control command can renew the
         * relay. Retried ON packets must not extend the hold deadline. */
        if (p->transaction > r->on_id && p->transaction > r->control_id) {
            r->on_id = p->transaction;
            if (now >= r->pause_until_ms) {
                r->on = true;
                r->relay_until_ms = now + RECEIVER_HOLD_MS;
            }
        }
    } else if ((p->kind == GW_PAUSE && p->length == 8) ||
               (p->kind == GW_RESUME && p->length == 0)) {
        if (p->transaction < r->control_id) {
            return error(r, p, o, 3);
        }
        if (p->transaction == r->control_id && p->kind != r->control_kind) {
            return error(r, p, o, 3);
        }
        if (p->transaction > r->control_id) {
            r->control_id = p->transaction;
            r->control_kind = p->kind;
            if (p->kind == GW_PAUSE) {
                uint64_t duration = gw_get64(p->data);
                if (duration > PAUSE_MS) {
                    duration = PAUSE_MS;
                }
                r->pause_until_ms = now + duration;
                gw_receiver_fail(r);
            } else {
                /* Resume permits a future fresh ON; it does not start the pump. */
                r->pause_until_ms = 0;
            }
        }
    } else if (p->kind == GW_SYNC && p->length == 0) {
        reply(r, p, o, GW_SYNC);
        gw_put64(o->data, rx_us);
        o->length = 16;
        /* Platform fills the predicted TX-end timestamp immediately before TX. */
        return 1;
    } else if (p->kind == GW_CORRECTION && p->length == 8) {
        if (p->transaction > r->clock_id) {
            r->clock_id = p->transaction;
            r->clock.offset_us = (int64_t)gw_get64(p->data);
            r->clock.valid = true;
            r->clock.synced_ms = now;
        }
    } else if (p->kind == GW_FETCH && p->length == 0) {
        if (!r->cache.valid || r->serial_pending || p->transaction != r->cache.transaction ||
            r->cache_error) {
            return error(r, p, o, 11);
        }
        reply(r, p, o, GW_PIECE);
        o->length = gw_piece(&r->cache, p->piece, o->data);
        return o->length ? 1 : error(r, p, o, 3);
    } else if (p->kind == GW_REQUEST) {
        if (p->length < 2 || !p->data[0] || p->data[0] == 255 || !p->data[1]) {
            return error(r, p, o, 3);
        }
        /* Return the cached outcome rather than execute a possible write twice. */
        if (gw_duplicate(&r->cache, p)) {
            if (r->serial_pending) {
                return error(r, p, o, 6);
            }
            result(r, p, o, rx_us);
            return 1;
        }
        if (p->transaction <= r->modbus_id || r->serial_pending) {
            return error(r, p, o, 3);
        }
        r->modbus_id = p->transaction;
        r->cache.valid = true;
        r->cache.session = p->session;
        r->cache.transaction = p->transaction;
        r->cache.request_length = p->length;
        memcpy(r->cache.request, p->data, p->length);
        r->cache.response_length = 0;
        r->cache.acquired_us = 0;
        r->cache_error = 0;
        r->request = *p;
        r->serial_pending = true;
        return 2;
    } else {
        return error(r, p, o, 3);
    }
    reply(r, p, o, p->kind);
    o->length = 9;
    o->data[0] = r->on;
    gw_put64(o->data + 1, r->pause_until_ms > now ? r->pause_until_ms - now : 0);
    return 1;
}
void gw_receiver_serial_done(gw_receiver *r, const uint8_t *frame, size_t n, uint64_t acquired_us,
                             gw_packet *out) {
    r->serial_pending = false;
    if (!gw_rtu_matches(r->cache.request, r->cache.request_length, frame, n)) {
        r->cache_error = 11;
    } else {
        r->cache.response_length = n - 2;
        memcpy(r->cache.response, frame, n - 2);
        r->cache.acquired_us = gw_utc(&r->clock, acquired_us);
    }
    result(r, &r->request, out, acquired_us);
}
