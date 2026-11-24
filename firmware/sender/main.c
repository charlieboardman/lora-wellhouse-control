#include <inttypes.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "hardware/watchdog.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "../gateway/radio.h"
#include "../gateway/sender.h"
#include "network.h"

static volatile uint64_t event_us;
static uint64_t take_event_us(void) {
    uint32_t flags = save_and_disable_interrupts();
    uint64_t event = event_us;
    event_us = 0;
    restore_interrupts(flags);
    return event ? event : time_us_64();
}
static void radio_irq(unsigned gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    event_us = time_us_64();
}
bool rf_transfer(const uint8_t *tx, uint8_t *rx, size_t n) {
    gpio_put(RADIO_CS, 0);
    int count = spi_write_read_blocking(spi1, tx, rx, n);
    gpio_put(RADIO_CS, 1);
    return count == (int)n;
}
bool rf_busy(void) {
    return gpio_get(RADIO_BUSY);
}
void rf_reset(bool high) {
    gpio_put(RADIO_RESET, high);
}
void rf_delay_us(unsigned us) {
    sleep_us(us);
}
uint64_t rf_time_us(void) {
    return time_us_64();
}
static void radio_pins(void) {
    gpio_init(RADIO_CS);
    gpio_put(RADIO_CS, 1);
    gpio_set_dir(RADIO_CS, GPIO_OUT);
    gpio_init(RADIO_RESET);
    gpio_put(RADIO_RESET, 0);
    gpio_set_dir(RADIO_RESET, GPIO_OUT);
    gpio_init(RADIO_BUSY);
    gpio_set_dir(RADIO_BUSY, GPIO_IN);
    gpio_init(RADIO_DIO1);
    gpio_set_dir(RADIO_DIO1, GPIO_IN);
    spi_init(spi1, SPI_BAUD_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(RADIO_SCK, GPIO_FUNC_SPI);
    gpio_set_function(RADIO_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(RADIO_MISO, GPIO_FUNC_SPI);
    gpio_set_irq_enabled_with_callback(RADIO_DIO1, GPIO_IRQ_EDGE_RISE, true, radio_irq);
}
static gw_sender sender;
int main(void) {
    gpio_init(FLOAT_GPIO);
    gpio_set_dir(FLOAT_GPIO, GPIO_IN);
    gpio_pull_up(FLOAT_GPIO);
    radio_pins();
    watchdog_enable(WATCHDOG_MS, false);
    stdio_init_all();
    bool net_ready = network_init();
    gw_sender_init(&sender, get_rand_64() | 1, time_us_64() / 1000);
    enum { IDLE, TX, RX } phase = IDLE;
    bool ready = false, led = false;
    unsigned failures = 0;
    uint64_t retry_ms = 0, refresh_ms = 0, health_ms = 0, sample_ms = 0, log_ms = 0, deadline = 0,
             quiet_until = 0, exchange_start = 0;
    uint8_t wire[GW_PACKET_MAX];
    gw_packet packet;
    printf("BOOT " FIRMWARE_VERSION " gateway sender ON=30s hold=130s network=%d\n", net_ready);
    for (;;) {
        uint64_t us = time_us_64(), now = us / 1000;
        if (net_ready) {
            network_poll(&sender, us);
        }
        if (now >= sample_ms) {
            gw_sender_sample(&sender, !gpio_get(FLOAT_GPIO), now);
            sample_ms = now + SAMPLE_INTERVAL_MS;
            if (!sender.grounded || now < sender.pause_until_ms) {
                led = false;
            }
        }
        if (!ready && now >= retry_ms) {
            ready = rf_init();
            retry_ms = time_us_64() / 1000 + RADIO_RETRY_MS;
            if (ready) {
                failures = 0;
                refresh_ms = health_ms = time_us_64() / 1000;
                printf("RADIO READY\n");
            } else if (++failures >= MAX_INIT_FAILURES) {
                watchdog_reboot(0, 0, 10);
                for (;;) {
                    tight_loop_contents();
                }
            }
        }
        bool fault = false;
        if (ready && phase == TX) {
            int result = rf_tx_poll();
            if (result == 1) {
                uint64_t done = take_event_us();
                gw_sender_sent(&sender, done);
                if (sender.out.kind == GW_ON) {
                    led = true;
                }
                event_us = 0;
                if (rf_receive()) {
                    phase = RX;
                    deadline = done / 1000 + REPLY_WINDOW_MS;
                } else {
                    fault = true;
                }
            } else if (result < 0 || now >= deadline) {
                fault = true;
            }
        } else if (ready && phase == RX) {
            size_t n = 0;
            int result = rf_rx_poll(wire, &n);
            if (result == 1) {
                uint64_t done = take_event_us();
                if (gw_decode(&packet, wire, n) && gw_sender_reply(&sender, &packet, done)) {
                    if (!rf_idle()) {
                        fault = true;
                    }
                    phase = IDLE;
                    quiet_until = time_us_64() / 1000 + RADIO_GUARD_MS;
                }
            } else if (result < 0 && result != -2) {
                fault = true;
            }
            if (phase == RX && now >= deadline) {
                gw_sender_timeout(&sender, now);
                if (!rf_idle()) {
                    fault = true;
                }
                phase = IDLE;
                quiet_until = now + RADIO_GUARD_MS;
            }
        }
        if (fault) {
            // A local radio failure does not prove the peer stopped transmitting.
            // Reserve the entire exchange window before scheduling another one.
            gw_sender_timeout(&sender, now);
            rf_abort();
            ready = false;
            led = false;
            phase = IDLE;
            quiet_until = exchange_start + TX_WAIT_MS + REPLY_WINDOW_MS + RADIO_GUARD_MS;
            retry_ms = now + RADIO_RETRY_MS;
        }
        if (ready && phase == IDLE && now >= quiet_until) {
            if (now - health_ms >= RADIO_HEALTH_MS) {
                ready = rf_health(false);
                health_ms = now;
            }
            if (ready && now - refresh_ms >= SENDER_RADIO_REFRESH_MS) {
                ready = rf_init();
                refresh_ms = now;
                health_ms = now;
            }
            if (ready && gw_sender_next(&sender, now, &packet)) {
                size_t n = gw_encode(&packet, wire);
                event_us = 0;
                exchange_start = now;
                if (n && rf_transmit(wire, n)) {
                    phase = TX;
                    deadline = now + TX_WAIT_MS;
                } else {
                    gw_sender_timeout(&sender, now);
                    ready = false;
                    quiet_until = now + TX_WAIT_MS + REPLY_WINDOW_MS;
                }
            }
            if (!ready) {
                rf_abort();
                led = false;
                retry_ms = now + RADIO_RETRY_MS;
            }
        }
        if (net_ready) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        }
        if (now >= log_ms) {
            printf("HEALTH ready=%d float=%d phase=%d queued=%u peer=%08lx%08lx"
                   " synced=%d t=%lu\n",
                   ready, sender.grounded, phase, sender.count,
                   (unsigned long)(sender.peer >> 32), (unsigned long)sender.peer,
                   sender.clock.valid, (unsigned long)now);
            log_ms = now + HEALTH_LOG_MS;
        }
        watchdog_update();
        sleep_ms(2);
    }
}
