#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../config.h"

typedef struct { uint64_t next_sample_ms; } sender_schedule_t;

static inline void sender_schedule_init(sender_schedule_t *s, uint64_t now) {
    s->next_sample_ms = now + SAMPLE_INTERVAL_MS;
}

// Once per slot, even if radio is unavailable. Never catch up with a TX burst.
static inline bool sender_take_slot(sender_schedule_t *s, uint64_t now) {
    if (now < s->next_sample_ms) return false;
    s->next_sample_ms = now + SAMPLE_INTERVAL_MS;
    return true;
}

static inline bool sender_should_transmit(bool grounded, bool radio_ready) {
    return grounded && radio_ready;
}
