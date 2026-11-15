#include "radio.h"
#include "../config.h"
#include <stddef.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

static uint8_t tx[32], rx[32];
static const uint8_t on_packet[ON_PACKET_SIZE] = {ON_PACKET_BYTES};

static bool wait_ready(void) {
    const uint64_t start = time_us_64();
    while (gpio_get(RADIO_BUSY)) {
        if (time_us_64() - start >= BUSY_TIMEOUT_US) return false;
        sleep_us(50);
    }
    return true;
}

// Only fixed-size stack/static buffers. SPI hardware stalls are covered by watchdog.
static bool command(const uint8_t *p, size_t n, size_t extra) {
    if (n > sizeof(tx) || extra > sizeof(tx) - n || !wait_ready()) return false;
    memset(tx, 0, n + extra);
    memcpy(tx, p, n);
    memset(rx, 0, n + extra);
    gpio_put(RADIO_CS, 0);
    const int count = spi_write_read_blocking(spi1, tx, rx, n + extra);
    gpio_put(RADIO_CS, 1);
    return count == (int)(n + extra) && wait_ready();
}
#define CMD(...) command((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}), 0)

static bool reg_read(uint16_t address, uint8_t *value) {
    const uint8_t p[] = {0x1d, (uint8_t)(address >> 8), (uint8_t)address};
    if (!command(p, sizeof(p), 2)) return false;
    *value = rx[4];
    return true;
}
static bool reg_write(uint16_t address, uint8_t value) {
    return CMD(0x0d, (uint8_t)(address >> 8), (uint8_t)address, value);
}
static bool clear_irq(void) { return CMD(0x02, 0xff, 0xff); }
static bool irq_read(uint16_t *flags) {
    const uint8_t op = 0x12;
    if (!command(&op, 1, 3)) return false;
    *flags = ((uint16_t)rx[2] << 8) | rx[3];
    return true;
}

void radio_gpio_init(void) {
    gpio_init(RADIO_CS); gpio_put(RADIO_CS, 1); gpio_set_dir(RADIO_CS, GPIO_OUT);
    gpio_init(RADIO_RESET); gpio_put(RADIO_RESET, 0); gpio_set_dir(RADIO_RESET, GPIO_OUT);
    gpio_init(RADIO_BUSY); gpio_set_dir(RADIO_BUSY, GPIO_IN);
    gpio_init(RADIO_DIO1); gpio_set_dir(RADIO_DIO1, GPIO_IN);
    spi_init(spi1, SPI_BAUD_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(RADIO_SCK, GPIO_FUNC_SPI);
    gpio_set_function(RADIO_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(RADIO_MISO, GPIO_FUNC_SPI);
}

void radio_abort(void) {
    gpio_put(RADIO_CS, 1);
    gpio_put(RADIO_RESET, 0); // Stop RF activity even if the command path failed.
}

bool radio_healthy(void) {
    const uint8_t status = 0xc0, errors = 0x17;
    if (!command(&status, 1, 1)) return false;
    const uint8_t s = rx[1], result = (s >> 1) & 7u;
    if ((s & 0x70u) != 0x20u || result == 3 || result == 4 || result == 5) return false;
    if (!command(&errors, 1, 3)) return false;
    if (rx[2] != 0 || rx[3] != 0) return false;
    // Status alone does not prove configuration is intact. Check the readable
    // LoRa sync word as well; periodic cold initialization restores all settings.
    uint8_t sync_hi = 0, sync_lo = 0;
    return reg_read(0x0740, &sync_hi) && reg_read(0x0741, &sync_lo) &&
        sync_hi == (uint8_t)(SYNC_WORD >> 8) && sync_lo == (uint8_t)SYNC_WORD;
}

bool radio_init(void) {
    radio_abort(); sleep_ms(2);
    gpio_put(RADIO_RESET, 1); sleep_ms(20);
    const uint32_t frf = (uint32_t)(((uint64_t)RF_FREQUENCY_HZ << 25) / 32000000u);
    const uint32_t tcxo_ticks = TCXO_STARTUP_MS * 64u;
    bool ok = CMD(0x80, 0) && // STDBY_RC
        CMD(0x97, TCXO_VOLTAGE_CODE, (uint8_t)(tcxo_ticks >> 16), (uint8_t)(tcxo_ticks >> 8), (uint8_t)tcxo_ticks) &&
        CMD(0x9d, 1) &&       // DIO2 automatically controls antenna switch.
        CMD(0x07, 0, 0) &&    // Clear oscillator startup errors before calibration.
        CMD(0x89, 0x7f) &&
        CMD(0x8a, 1) &&       // LoRa packet type
        CMD(0x98, 0xe1, 0xe9) && // 902-928 MHz image calibration
        CMD(0x86, (uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf) &&
        CMD(0x8b, LORA_SF, LORA_BW_CODE, LORA_CR_CODE, LORA_LDRO) &&
        CMD(0x8f, 0, 0) &&
        CMD(0x8c, 0, PREAMBLE_SYMBOLS, 0, ON_PACKET_SIZE, 1, 0) && // Explicit header, CRC, normal IQ
        CMD(0x95, 4, 7, 0, 1) && // SX1262 PA configuration
        CMD(0x8e, (uint8_t)TX_POWER_DBM, 4) && // 200 us ramp
        CMD(0x93, 0x20) &&    // Fall back to STDBY_RC after TX.
        CMD(0x08, 0x02, 0x01, 0x02, 0x01, 0, 0, 0, 0) && // TX_DONE / TIMEOUT
        CMD(0x0d, 0x07, 0x40, (uint8_t)(SYNC_WORD >> 8), (uint8_t)SYNC_WORD);
    uint8_t clamp = 0, modulation = 0;
    // Semtech SX1262 PA clamp and non-500 kHz modulation-quality workarounds.
    ok = ok && reg_read(0x08d8, &clamp) && reg_write(0x08d8, clamp | 0x1e) &&
        reg_read(0x0889, &modulation) && reg_write(0x0889, modulation | 0x04) &&
        clear_irq() && radio_healthy();
    if (!ok) radio_abort();
    return ok;
}

bool radio_send_on(void) {
    uint8_t buffer[2 + ON_PACKET_SIZE] = {0x0e, 0};
    memcpy(buffer + 2, on_packet, sizeof(on_packet));
    const uint32_t ticks = TX_TIMEOUT_MS * 64u;
    bool ok = CMD(0x80, 0) && clear_irq() && command(buffer, sizeof(buffer), 0) &&
        CMD(0x83, (uint8_t)(ticks >> 16), (uint8_t)(ticks >> 8), (uint8_t)ticks);
    bool done = false;
    const uint64_t start = time_us_64();
    while (ok && time_us_64() - start < TX_WAIT_MS * 1000u) {
        uint16_t flags = 0;
        ok = irq_read(&flags);
        if (!ok || (flags & 0x0200u)) { ok = false; break; }
        if (flags & 1u) { done = true; break; }
        sleep_ms(2);
    }
    ok = ok && done && CMD(0x80, 0) && clear_irq() && radio_healthy();
    if (!ok) radio_abort();
    return ok;
}
