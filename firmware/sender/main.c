#include <inttypes.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "../config.h"
#include "radio.h"
#include "sender_logic.h"

static uint64_t ms(void) { return time_us_64() / 1000u; }
static bool led_available, led_state;
static void led_set(bool on) {
    if (led_available && on != led_state) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        led_state = on;
    }
}

int main(void) {
    gpio_init(FLOAT_GPIO);
    gpio_set_dir(FLOAT_GPIO, GPIO_IN);
    gpio_pull_up(FLOAT_GPIO); // Open/disconnected switch -> no ON commands.
    radio_gpio_init();        // Hold radio reset low until configured.
    watchdog_enable(WATCHDOG_MS, false); // Also runs if CPU/debugger stops.
    stdio_init_all();         // USB logging optional; never wait for a host.
    led_available = cyw43_arch_init() == 0; // LED access only; Wi-Fi/BT never enabled.
    if (led_available) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    sender_schedule_t schedule;
    sender_schedule_init(&schedule, ms());
    bool ready = false, refresh_requested = false;
    unsigned init_failures = 0;
    uint64_t next_try = 0, last_health = 0, last_refresh = 0, last_log = 0, sent = 0, silent = 0;
    printf("BOOT " FIRMWARE_VERSION " pico2_w ON-only GP%u interval=%" PRIu64 " hold=%" PRIu64
           " SF%u TX=%d dBm watchdog_reset=%d\n", FLOAT_GPIO,
           SAMPLE_INTERVAL_MS, RECEIVER_HOLD_MS, LORA_SF, TX_POWER_DBM, watchdog_caused_reboot());
    for (;;) {
        uint64_t now = ms();
        if (refresh_requested || (!ready && now >= next_try)) {
            refresh_requested = false;
            ready = radio_init();
            next_try = ms() + RADIO_RETRY_MS;
            last_health = ms();
            if (ready) {
                init_failures = 0;
                last_refresh = last_health;
                printf("RADIO READY t=%" PRIu64 "\n", ms());
            } else {
                printf("RADIO init failed %u\n", ++init_failures);
                if (init_failures >= MAX_INIT_FAILURES) {
                    radio_abort(); led_set(false);
                    watchdog_reboot(0, 0, 10);
                    for (;;) tight_loop_contents();
                }
            }
        }
        now = ms();
        if (ready && now - last_health >= RADIO_HEALTH_MS) {
            ready = radio_healthy();
            last_health = ms();
            if (!ready) next_try = ms() + RADIO_RETRY_MS;
        }
        now = ms();
        if (sender_take_slot(&schedule, now)) {
            // Exactly one sample per slot. No input-edge transmissions or debounce.
            const bool grounded = !gpio_get(FLOAT_GPIO);
            if (sender_should_transmit(grounded, ready)) {
                ready = radio_send_on();
                led_set(ready);
                if (ready) ++sent;
                else next_try = ms() + RADIO_RETRY_MS;
                printf("TX ON ok=%d sampled=%" PRIu64 " done=%" PRIu64 "\n", ready, now, ms());
            } else {
                led_set(false); ++silent;
                printf("SILENT grounded=%d ready=%d t=%" PRIu64 "\n", grounded, ready, now);
            }
            // TX is synchronous and finished here. Reconfigure on the next loop,
            // after feeding the watchdog, without changing the sample schedule.
            if (ready && ms() - last_refresh >= SENDER_RADIO_REFRESH_MS) {
                refresh_requested = true;
                printf("RADIO scheduled refresh t=%" PRIu64 "\n", ms());
            }
        }
        if (!ready) { radio_abort(); led_set(false); }
        now = ms();
        if (now - last_log >= HEALTH_LOG_MS) {
            printf("HEALTH version=" FIRMWARE_VERSION " tx_dbm=%d ready=%d sent=%" PRIu64 " silent=%" PRIu64 " t=%" PRIu64 "\n",
                   TX_POWER_DBM, ready, sent, silent, now);
            last_log = now;
        }
        watchdog_update(); // Feed only after a complete successful loop iteration.
        sleep_ms(5);
    }
}
