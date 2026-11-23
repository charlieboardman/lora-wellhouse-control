#pragma once
#include "protocol.h"
#include "clock.h"
#include "../config.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t boot, session, pause_until_ms, relay_until_ms, reply_deadline_us;
    uint32_t control_id, on_id, modbus_id, clock_id;
    uint8_t control_kind, cache_error;
    bool on, serial_pending;
    gw_cache cache;
    gw_packet request;
    gw_clock clock;
} gw_receiver;
void gw_receiver_init(gw_receiver *r, uint64_t boot);
void gw_receiver_tick(gw_receiver *r, uint64_t now);
/* 0=no reply, 1=reply now, 2=execute request.data on serial asynchronously. */
int gw_receiver_request(gw_receiver *r, const gw_packet *p, uint64_t rx_us, gw_packet *out);
void gw_receiver_serial_done(gw_receiver *r, const uint8_t *frame, size_t n, uint64_t acquired_us,
                             gw_packet *out);
void gw_receiver_fail(gw_receiver *r);
#ifdef __cplusplus
}
#endif
