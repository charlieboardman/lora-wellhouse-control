#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct {bool valid; int64_t offset_us; uint64_t synced_ms;} gw_clock;
static inline uint64_t gw_utc(const gw_clock *c,uint64_t mono_us) {
    return c->valid ? (uint64_t)((int64_t)mono_us+c->offset_us):0;
}
static inline void gw_clock_set(gw_clock *c,uint64_t mono_us,uint64_t utc_us) {
    c->valid=true;c->offset_us=(int64_t)utc_us-(int64_t)mono_us;c->synced_ms=mono_us/1000;
}
