#include "sender.h"
static uint32_t next_id(gw_sender *s) {
    return ++s->sequence;
}
static void finish(gw_sender *s, uint8_t error) {
    if (!s->active) {
        return;
    }
    s->completed_id = s->active_id;
    s->last_tcp_id = s->job.tcp_id;
    s->last_result_client = s->job.client;
    s->last_acquired_us = s->acquired_us;
    s->last_result_synced = s->result_synced;
    s->last_sync_age = s->receiver_sync_age;
    s->last_unit = s->job.data[0];
    s->last_function = s->job.data[1];
    s->last_error = error;
    gw_deliver(s->job.client, s->job.tcp_id, error ? s->job.data : s->response,
               error ? 2 : s->response_used, error);
    s->active = false;
    s->fetching = false;
    s->response_used = 0;
}
/* A changed receiver boot invalidates an uncertain write's cached outcome.
 * Fail it instead of replaying it against the newly booted receiver. */
static void receiver_restarted(gw_sender *s) {
    finish(s, 11);
    s->retry_pending = false;
}

void gw_sender_init(gw_sender *s, uint64_t session, uint64_t now) {
    memset(s, 0, sizeof(*s));
    s->session = session;
    s->next_on_ms = now + ON_INTERVAL_MS;
}
void gw_sender_sample(gw_sender *s, bool grounded, uint64_t now) {
    if (s->active && now >= s->active_deadline_ms) {
        if (s->retry_pending && (s->retry.kind == GW_REQUEST || s->retry.kind == GW_FETCH)) {
            s->retry_pending = false;
        }
        finish(s, 11);
    }
    s->grounded = grounded;
    if (now >= s->next_on_ms) {
        s->next_on_ms = now + ON_INTERVAL_MS;
        s->on_pending = grounded;
    }
    if (!grounded || now < s->pause_until_ms) {
        s->on_pending = false;
    }
}
void gw_sender_ntp(gw_sender *s, uint64_t mono_us, uint64_t utc_us) {
    gw_clock_set(&s->clock, mono_us, utc_us);
    ++s->clock_revision;
    s->sync_pending = true;
    s->sync_retry_ms = 0;
    s->correction_pending = false;
    if (s->retry_pending && (s->retry.kind == GW_SYNC || s->retry.kind == GW_CORRECTION)) {
        s->retry_pending = false;
    }
}
uint8_t gw_sender_submit(gw_sender *s, uint32_t client, uint16_t tcp_id, const uint8_t *p, size_t n,
                         uint64_t now) {
    if (n < 2 || n > GW_DATA_MAX || !p[0] || p[0] == 255 || !p[1]) {
        return 3;
    }
    if (s->count == GW_QUEUE_SIZE) {
        return 6;
    }
    gw_job *j = &s->jobs[(s->head + s->count) % GW_QUEUE_SIZE];
    j->client = client;
    j->tcp_id = tcp_id;
    j->n = n;
    j->queued_ms = now;
    memcpy(j->data, p, n);
    ++s->count;
    return 0;
}
bool gw_sender_control(gw_sender *s, bool pause, uint32_t client, uint64_t now) {
    if (s->control_pending) {
        return false;
    }
    s->pause_until_ms = pause ? now + PAUSE_MS : 0;
    s->on_pending = false;
    s->control_pending = true;
    s->control_kind = pause ? GW_PAUSE : GW_RESUME;
    s->control_client = client;
    s->control_id = next_id(s);
    s->control_deadline_ms = now + 120000;
    return true;
}
static void base(gw_sender *s, gw_packet *p, uint8_t kind, uint32_t id) {
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->session = s->session;
    p->receiver_boot = s->peer;
    p->transaction = id;
}
bool gw_sender_next(gw_sender *s, uint64_t now, gw_packet *p) {
    if (s->have_out) {
        return false;
    }
    if (s->control_pending && now >= s->control_deadline_ms) {
        if (s->retry_pending && s->retry.transaction == s->control_id) {
            s->retry_pending = false;
        }
        s->control_pending = false;
        gw_control_done(s->control_client, false);
    }
    /* One exchange at a time. Control and fresh pump heartbeats precede
     * retries, clock synchronization, and queued Modbus work. */
    s->out_attempt = 0;
    if (!s->peer && s->retry_pending && s->retry.kind == GW_HELLO) {
        *p = s->retry;
        s->retry_pending = false;
        s->out_attempt = 1;
    } else if (!s->peer) {
        base(s, p, GW_HELLO, next_id(s));
    } else if (s->control_pending && !(s->retry_pending && s->retry.transaction == s->control_id)) {
        base(s, p, s->control_kind, s->control_id);
        if (p->kind == GW_PAUSE) {
            gw_put64(p->data, s->pause_until_ms > now ? s->pause_until_ms - now : 0);
            p->length = 8;
        }
    } else if (s->on_pending && s->grounded && now >= s->pause_until_ms) {
        base(s, p, GW_ON, next_id(s));
        s->on_pending = false;
    } else if (s->retry_pending) {
        *p = s->retry;
        s->retry_pending = false;
        s->out_attempt = 1;
        if (p->kind == GW_ON && (!s->grounded || now < s->pause_until_ms)) {
            return false;
        }
        if (p->kind == GW_PAUSE) {
            gw_put64(p->data, s->pause_until_ms > now ? s->pause_until_ms - now : 0);
        }
    } else if (s->correction_pending) {
        base(s, p, GW_CORRECTION, next_id(s));
        gw_put64(p->data, (uint64_t)s->correction);
        p->length = 8;
        s->correction_pending = false;
    } else if (s->sync_pending && s->clock.valid && now >= s->sync_retry_ms) {
        base(s, p, GW_SYNC, next_id(s));
        s->sync_pending = false;
        s->sync_revision = s->clock_revision;
    } else {
        while (!s->active && s->count) {
            s->job = s->jobs[s->head];
            s->head = (s->head + 1) % GW_QUEUE_SIZE;
            --s->count;
            if (now - s->job.queued_ms > CLIENT_QUEUE_MS) {
                gw_deliver(s->job.client, s->job.tcp_id, s->job.data, 2, 6);
            } else {
                s->active = true;
                s->active_deadline_ms = now + 180000;
                s->active_id = next_id(s);
                s->response_used = 0;
                s->fetching = false;
                s->acquired_us = 0;
                s->result_synced = false;
                s->receiver_sync_age = UINT32_MAX;
            }
        }
        if (!s->active) {
            return false;
        }
        base(s, p, s->fetching ? GW_FETCH : GW_REQUEST, s->active_id);
        if (s->fetching) {
            p->piece = s->next_piece;
        } else {
            p->length = s->job.n;
            memcpy(p->data, s->job.data, p->length);
        }
    }
    s->out = *p;
    s->have_out = true;
    return true;
}
void gw_sender_sent(gw_sender *s, uint64_t tx_done_us) {
    if (s->out.kind == GW_SYNC) {
        s->sync_t1_us = gw_utc(&s->clock, tx_done_us);
    }
}
bool gw_sender_reply(gw_sender *s, const gw_packet *p, uint64_t rx_done_us) {
    if (!s->have_out || p->session != s->session || p->transaction != s->out.transaction) {
        return false;
    }
    if (s->out.kind != GW_HELLO && p->receiver_boot != s->peer) {
        receiver_restarted(s);
        s->peer = 0;
        s->have_out = false;
        return true;
    }
    if ((s->out.kind == GW_REQUEST || s->out.kind == GW_FETCH) && !s->active) {
        s->have_out = false;
        return true;
    }
    if (s->out.kind == GW_HELLO) {
        if (p->kind != GW_HELLO || !p->receiver_boot || p->length) {
            return false;
        }
        /* peer may have been cleared by a failed priority heartbeat while a
         * Modbus retry was waiting. known_peer survives that reconnect. */
        if (s->known_peer && s->known_peer != p->receiver_boot) {
            receiver_restarted(s);
            s->correction_pending = false;
        }
        s->known_peer = p->receiver_boot;
        if (s->peer != p->receiver_boot) {
            s->peer = p->receiver_boot;
            s->sync_pending = s->clock.valid;
            s->receiver_clock_synced = false;
            if (s->pause_until_ms > rx_done_us / 1000 && !s->control_pending) {
                s->control_pending = true;
                s->control_kind = GW_PAUSE;
                s->control_client = 0;
                s->control_id = next_id(s);
                s->control_deadline_ms = rx_done_us / 1000 + 120000;
            }
        }
    } else if (p->kind == GW_ERROR) {
        if (s->out.kind == GW_REQUEST || s->out.kind == GW_FETCH) {
            finish(s, p->length ? p->data[0] : 11);
        } else if (s->out.kind == GW_PAUSE || s->out.kind == GW_RESUME) {
            gw_control_done(s->control_client, false);
            s->control_pending = false;
        }
    } else if (s->out.kind == GW_REQUEST && (p->kind == GW_RESULT || p->kind == GW_READY)) {
        if (p->length < 16) {
            return false;
        }
        size_t n = ((size_t)p->data[0] << 8) | p->data[1];
        if (n < 2 || n > 254 || p->data[15] != (n + GW_DATA_MAX - 1) / GW_DATA_MAX) {
            return false;
        }
        s->response_size = n;
        s->acquired_us = gw_get64(p->data + 2);
        s->result_synced = p->data[10] != 0;
        s->receiver_sync_age = 0;
        for (unsigned i = 11; i < 15; i++) {
            s->receiver_sync_age = (s->receiver_sync_age << 8) | p->data[i];
        }
        if (p->kind == GW_RESULT) {
            if (p->length != 16 + n) {
                return false;
            }
            memcpy(s->response, p->data + 16, n);
            s->response_used = n;
            finish(s, 0);
        } else {
            if (p->length != 16) {
                return false;
            }
            s->fetching = true;
            s->next_piece = 0;
        }
    } else if (s->out.kind == GW_FETCH && p->kind == GW_PIECE && p->piece == s->out.piece) {
        size_t remaining = s->response_size - s->response_used;
        size_t expected = remaining > GW_DATA_MAX ? GW_DATA_MAX : remaining;
        if (p->length != expected) {
            return false;
        }
        memcpy(s->response + s->response_used, p->data, p->length);
        s->response_used += p->length;
        ++s->next_piece;
        if (s->response_used == s->response_size) {
            finish(s, 0);
        }
    } else if (s->out.kind == GW_SYNC && p->kind == GW_SYNC && p->length == 16) {
        if (s->sync_revision == s->clock_revision) {
            int64_t t1 = (int64_t)s->sync_t1_us, t2 = (int64_t)gw_get64(p->data);
            int64_t t3 = (int64_t)gw_get64(p->data + 8),
                    t4 = (int64_t)gw_utc(&s->clock, rx_done_us);
            int64_t delay = (t4 - t1) - (t3 - t2);
            if (delay >= -100000 && delay <= 1000000) {
                s->correction = ((t1 - t2) + (t4 - t3)) / 2;
                s->correction_pending = true;
            } else {
                s->sync_pending = true;
                s->sync_retry_ms = rx_done_us / 1000 + 60000;
            }
        } else {
            s->sync_pending = true;
            s->sync_retry_ms = rx_done_us / 1000 + 60000;
        }
    } else if (p->kind == s->out.kind && (p->kind == GW_ON || p->kind == GW_PAUSE ||
                                          p->kind == GW_RESUME || p->kind == GW_CORRECTION)) {
        if (p->length != 9) {
            return false;
        }
        if (p->length) {
            s->receiver_on = p->data[0] != 0;
            s->receiver_status_ms = rx_done_us / 1000;
        }
        if (p->length == 9) {
            s->receiver_pause_until_ms = rx_done_us / 1000 + gw_get64(p->data + 1);
        }
        if (p->kind == GW_ON) {
            s->last_on_ack_ms = rx_done_us / 1000;
        }
        if (p->kind == GW_CORRECTION) {
            s->receiver_clock_synced = true;
            s->receiver_clock_sync_ms = rx_done_us / 1000;
        }
        if (p->kind == GW_PAUSE || p->kind == GW_RESUME) {
            s->control_pending = false;
            gw_control_done(s->control_client, true);
            if (p->kind == GW_RESUME && s->grounded) {
                s->on_pending = true;
            }
        }
    } else {
        return false;
    }
    s->have_out = false;
    return true;
}
void gw_sender_timeout(gw_sender *s, uint64_t now) {
    if (!s->have_out) {
        return;
    }
    /* Preserve transaction identity for the single retry. Never overwrite a
     * waiting retry when a higher-priority exchange fails. */
    if (!s->out_attempt && !s->retry_pending) {
        s->retry = s->out;
        s->retry_pending = true;
    } else {
        if (s->out.kind == GW_REQUEST || s->out.kind == GW_FETCH) {
            finish(s, 11);
        }
        if (s->out.kind == GW_PAUSE || s->out.kind == GW_RESUME) {
            s->control_pending = false;
            gw_control_done(s->control_client, false);
        }
        if (s->out.kind == GW_CORRECTION || s->out.kind == GW_SYNC) {
            s->sync_pending = true;
            s->sync_retry_ms = now + 60000;
        }
        /* Require a handshake, but retain known_peer so a reboot discovered
         * after reconnect cannot replay an uncertain write. */
        s->peer = 0;
    }
    s->have_out = false;
}
