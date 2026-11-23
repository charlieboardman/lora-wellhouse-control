#pragma once
#include "protocol.h"
#include "clock.h"
#include "../config.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t client;
    uint16_t tcp_id;
    size_t n;
    uint64_t queued_ms;
    uint8_t data[GW_DATA_MAX];
} gw_job;
typedef struct {
    uint64_t session;
    uint64_t peer;       /* Current handshake target; zero requires HELLO. */
    uint64_t known_peer; /* Last receiver boot, retained across reconnects. */
    uint64_t sync_retry_ms, next_on_ms, pause_until_ms, control_deadline_ms, active_deadline_ms;
    uint32_t sequence;
    bool grounded, on_pending, control_pending, sync_pending, correction_pending;
    bool have_out;      /* out owns the single in-flight radio exchange. */
    bool retry_pending; /* retry keeps its identity even across priority traffic. */
    bool active;        /* job owns the current Modbus operation. */
    bool fetching;      /* The active operation is assembling response pieces. */
    uint8_t control_kind, out_attempt, next_piece;
    uint32_t control_client, control_id, active_id;
    gw_job jobs[GW_QUEUE_SIZE], job;
    unsigned head, count;
    gw_packet out, retry;
    uint8_t response[GW_RESPONSE_MAX];
    size_t response_size, response_used;
    uint64_t acquired_us, last_on_ack_ms, receiver_sync_age, receiver_status_ms,
        receiver_pause_until_ms;
    bool result_synced, receiver_on, receiver_clock_synced;
    uint64_t receiver_clock_sync_ms;
    gw_clock clock;
    unsigned clock_revision, sync_revision;
    uint64_t sync_t1_us;
    int64_t correction;
    uint32_t completed_id, last_result_client;
    uint16_t last_tcp_id;
    uint8_t last_unit, last_function, last_error;
    uint64_t last_acquired_us, last_sync_age;
    bool last_result_synced;
} gw_sender;
/* Implemented by the host/network layer; callbacks occur only from the app loop. */
void gw_deliver(uint32_t client, uint16_t tcp_id, const uint8_t *data, size_t n, uint8_t error);
void gw_control_done(uint32_t client, bool confirmed);
void gw_sender_init(gw_sender *s, uint64_t session, uint64_t now);
void gw_sender_sample(gw_sender *s, bool grounded, uint64_t now);
void gw_sender_ntp(gw_sender *s, uint64_t mono_us, uint64_t utc_us);
uint8_t gw_sender_submit(gw_sender *s, uint32_t client, uint16_t tcp_id, const uint8_t *p, size_t n,
                         uint64_t now);
bool gw_sender_control(gw_sender *s, bool pause, uint32_t client, uint64_t now);
bool gw_sender_next(gw_sender *s, uint64_t now, gw_packet *p);
void gw_sender_sent(gw_sender *s, uint64_t tx_done_us);
bool gw_sender_reply(gw_sender *s, const gw_packet *p, uint64_t rx_done_us);
void gw_sender_timeout(gw_sender *s, uint64_t now);
#ifdef __cplusplus
}
#endif
