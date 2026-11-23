#include <initializer_list>
#include <stdio.h>
#include <inttypes.h>
#include "../gateway/receiver.h"
#include "../gateway/radio.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static constexpr int CS = 14, BUSY = 13, RESET = 12, SCK = 35, MOSI = 36, MISO = 37;
static uint64_t ms() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}
static void set(int p, int v) {
    gpio_set_level(static_cast<gpio_num_t>(p), v);
}
static void out(int p, int v) {
    set(p, v);
    gpio_set_direction(static_cast<gpio_num_t>(p), GPIO_MODE_OUTPUT);
}
static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static bool indicated = false;
static void outputs(bool on) {
    set(1, on);
    if (on == indicated) {
        return;
    }
    rmt_symbol_word_t symbols[25] = {};
    const uint32_t color = on ? 0x180000 : 0; // GRB: green
    for (int i = 0; i < 24; i++) {
        bool bit = color & (1u << (23 - i));
        symbols[i].level0 = 1;
        symbols[i].duration0 = bit ? 8 : 4;
        symbols[i].level1 = 0;
        symbols[i].duration1 = bit ? 4 : 8;
    }
    symbols[24].duration0 = 500;
    symbols[24].duration1 = 500;
    rmt_transmit_config_t cfg = {};
    esp_err_t err = rmt_transmit(led_channel, led_encoder, symbols, sizeof(symbols), &cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(led_channel, 50);
    }
    if (err != ESP_OK) {
        set(1, 0);
        esp_restart();
    }
    indicated = on;
    printf("RELAY %d t=%" PRIu64 "\n", on, ms());
}
static void led_init() {
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = GPIO_NUM_38;
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = 10000000;
    cfg.mem_block_symbols = 64;
    cfg.trans_queue_depth = 1;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &led_channel));
    rmt_copy_encoder_config_t enc = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));
    indicated = true;
    outputs(false);
}

static spi_device_handle_t spi;
extern "C" bool rf_transfer(const uint8_t *tx, uint8_t *rx, size_t n) {
    // DMA-free ESP32 SPI transfers are limited by the hardware FIFO. Keep CS
    // asserted across chunks so the SX1262 sees one command of up to 258 bytes.
    bool ok = true;
    set(CS, 0);
    for (size_t offset = 0; offset < n && ok; offset += 32) {
        size_t count = n - offset;
        if (count > 32) {
            count = 32;
        }
        spi_transaction_t t = {};
        t.length = count * 8;
        t.tx_buffer = tx + offset;
        t.rx_buffer = rx + offset;
        ok = spi_device_polling_transmit(spi, &t) == ESP_OK;
    }
    set(CS, 1);
    return ok;
}
extern "C" bool rf_busy(void) {
    return gpio_get_level(static_cast<gpio_num_t>(BUSY));
}
extern "C" void rf_reset(bool high) {
    set(RESET, high);
}
extern "C" uint64_t rf_time_us(void) {
    return static_cast<uint64_t>(esp_timer_get_time());
}
extern "C" void rf_delay_us(unsigned us) {
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    } else {
        esp_rom_delay_us(us);
    }
}
static gw_receiver receiver;
static uint8_t serial_frame[GW_RESPONSE_MAX];
static size_t serial_used;
static bool serial_overflow;
static uint64_t serial_deadline, serial_last_us;
static void serial_begin(uint64_t now) {
    uint8_t frame[GW_DATA_MAX + 2];
    size_t n = receiver.request.length;
    memcpy(frame, receiver.request.data, n);
    uint16_t crc = gw_crc(frame, n);
    frame[n++] = (uint8_t)crc;
    frame[n++] = (uint8_t)(crc >> 8);
    uart_flush_input(UART_NUM_1);
    serial_used = 0;
    serial_overflow = false;
    serial_last_us = 0;
    if (uart_write_bytes(UART_NUM_1, frame, n) != (int)n) {
        serial_overflow = true;
    }
    serial_deadline = now + SERIAL_RESPONSE_MS * 1000;
}
static bool serial_poll(uint64_t now, gw_packet *reply) {
    if (!receiver.serial_pending) {
        return false;
    }
    uint8_t incoming[64];
    int n = uart_read_bytes(UART_NUM_1, incoming, sizeof(incoming), 0);
    if (n > 0) {
        serial_last_us = now;
        if (serial_used + (size_t)n > sizeof(serial_frame)) {
            serial_overflow = true;
        } else {
            memcpy(serial_frame + serial_used, incoming, (size_t)n);
            serial_used += (size_t)n;
        }
    }
    if (now >= serial_deadline ||
        (serial_last_us && now - serial_last_us >= SERIAL_GAP_MS * 1000)) {
        gw_receiver_serial_done(&receiver, serial_frame, serial_overflow ? 0 : serial_used,
                                serial_last_us ? serial_last_us : now, reply);
        return true;
    }
    return false;
}
extern "C" void app_main(void) {
    // Establish fail-off outputs before initializing peripherals.
    for (int p : {1, 2, 41, 42, 45, 46}) {
        out(p, 0);
    }
    esp_task_wdt_config_t wd = {};
    wd.timeout_ms = WATCHDOG_MS;
    wd.idle_core_mask = 3;
    wd.trigger_panic = true;
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wd));
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    led_init();
    out(CS, 1);
    out(RESET, 0);
    gpio_set_direction(static_cast<gpio_num_t>(BUSY), GPIO_MODE_INPUT);
    spi_bus_config_t bus = {};
    bus.mosi_io_num = MOSI;
    bus.miso_io_num = MISO;
    bus.sclk_io_num = SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 32;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = SPI_BAUD_HZ;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));
    uart_config_t serial = {};
    serial.baud_rate = SERIAL_BAUD;
    serial.data_bits = UART_DATA_8_BITS;
    serial.parity = SERIAL_PARITY == 0   ? UART_PARITY_DISABLE
                    : SERIAL_PARITY == 1 ? UART_PARITY_ODD
                                         : UART_PARITY_EVEN;
    serial.stop_bits = SERIAL_STOP_BITS == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    serial.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    serial.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 512, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &serial));
    // Board uses an inverter/isolator for automatic direction; no RTS GPIO.
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uint64_t boot;
    esp_fill_random(&boot, sizeof(boot));
    gw_receiver_init(&receiver, boot | 1);
    bool ready = false, transmitting = false, pending = false;
    unsigned failures = 0;
    uint64_t retry_ms = 0, refresh_ms = 0, health_ms = 0, log_ms = 0, tx_deadline = 0;
    gw_packet request, reply;
    uint8_t wire[GW_PACKET_MAX];
    printf("BOOT " FIRMWARE_VERSION " gateway receiver hold=130s\n");
    for (;;) {
        uint64_t now = rf_time_us();
        gw_receiver_tick(&receiver, now / 1000);
        outputs(receiver.on);
        if (!ready && now / 1000 >= retry_ms) {
            ready = rf_init() && rf_receive();
            retry_ms = ms() + RADIO_RETRY_MS;
            if (ready) {
                failures = 0;
                refresh_ms = health_ms = ms();
                printf("RADIO READY\n");
            } else if (++failures >= MAX_INIT_FAILURES) {
                esp_restart();
            }
        }
        if (ready && transmitting) {
            int result = rf_tx_poll();
            if (result == 1) {
                transmitting = false;
                ready = rf_receive();
            } else if (result < 0 || now >= tx_deadline) {
                ready = false;
            }
        }
        if (ready && !transmitting) {
            if (serial_poll(now, &reply)) {
                pending = true;
            }
            if (!pending && !receiver.serial_pending) {
                size_t n = 0;
                int result = rf_rx_poll(wire, &n);
                if (result == 1 && gw_decode(&request, wire, n)) {
                    int action = gw_receiver_request(&receiver, &request, rf_time_us(), &reply);
                    if (action == 1) {
                        pending = true;
                    } else if (action == 2) {
                        serial_begin(rf_time_us());
                    }
                } else if (result < 0 && result != -2) {
                    ready = false;
                }
            }
            if (pending) {
                pending = false;
                if (rf_time_us() <= receiver.reply_deadline_us) {
                    if (reply.kind == GW_SYNC) {
                        gw_put64(reply.data + 8,
                                 rf_time_us() + rf_airtime_us(GW_HEADER_SIZE + reply.length));
                    }
                    size_t n = gw_encode(&reply, wire);
                    ready = n && rf_transmit_before(wire, n, receiver.reply_deadline_us);
                    transmitting = ready;
                    tx_deadline = rf_time_us() + TX_WAIT_MS * 1000;
                }
            }
            if (ready && !transmitting && !receiver.serial_pending &&
                ms() - health_ms >= RADIO_HEALTH_MS) {
                ready = rf_health(true);
                health_ms = ms();
            }
            if (ready && !transmitting && !receiver.serial_pending && !receiver.on &&
                ms() - refresh_ms >= SENDER_RADIO_REFRESH_MS) {
                ready = rf_init() && rf_receive();
                refresh_ms = health_ms = ms();
            }
        }
        if (!ready) {
            // Drive the relay off before radio recovery or serial cleanup.
            gw_receiver_fail(&receiver);
            outputs(false);
            rf_abort();
            transmitting = false;
            pending = false;
            if (receiver.serial_pending) {
                gw_receiver_serial_done(&receiver, serial_frame, 0, now, &reply);
            }
        }
        // Recheck with fresh time after peripheral calls, not the loop's old timestamp.
        gw_receiver_tick(&receiver, ms());
        outputs(receiver.on);
        if (ms() >= log_ms) {
            printf("HEALTH ready=%d on=%d serial=%d synced=%d free=%u\n", ready, receiver.on,
                   receiver.serial_pending, receiver.clock.valid,
                   (unsigned)esp_get_free_heap_size());
            log_ms = ms() + HEALTH_LOG_MS;
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
