#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../config.h"

struct Control {
    bool on = false;
    uint64_t deadline = 0;
    void fail() { on = false; deadline = 0; }
    bool accept(const uint8_t *p, size_t n, uint64_t now) {
        static constexpr uint8_t expected[] = {ON_PACKET_BYTES};
        if (n != sizeof(expected) || memcmp(p, expected, sizeof(expected)) != 0) return false;
        on = true;
        deadline = now + RECEIVER_HOLD_MS;
        return true;
    }
    void tick(uint64_t now) { if (on && now >= deadline) fail(); }
};
