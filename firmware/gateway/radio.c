#include "radio.h"
#include "../config.h"
#include <string.h>
static uint8_t tx[260],rx[260];
static bool wait_ready(void) {
    const uint64_t start = rf_time_us();
    while (rf_busy()) {
        if (rf_time_us() - start >= BUSY_TIMEOUT_US) return false;
        rf_delay_us(50);
    }
    return true;
}

// Only fixed-size stack/static buffers. SPI hardware stalls are covered by watchdog.
static bool command(const uint8_t *p, size_t n, size_t extra) {
    if (n > sizeof(tx) || extra > sizeof(tx) - n || !wait_ready()) return false;
    memset(tx, 0, n + extra);
    memcpy(tx, p, n);
    memset(rx, 0, n + extra);
    return rf_transfer(tx, rx, n + extra) && wait_ready();
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

void rf_abort(void) { rf_reset(false); }
bool rf_health(bool receiving) {
    const uint8_t status = 0xc0, errors = 0x17;
    if (!command(&status, 1, 1)) return false;
    const uint8_t s = rx[1], result = (s >> 1) & 7u;
    if ((s & 0x70u) != (receiving ? 0x50u : 0x20u) || result == 3 || result == 4 || result == 5) return false;
    if (!command(&errors, 1, 3)) return false;
    if (rx[2] != 0 || rx[3] != 0) return false;
    // Status alone does not prove configuration is intact. Check the readable
    // LoRa sync word as well; periodic cold initialization restores all settings.
    uint8_t sync_hi = 0, sync_lo = 0;
    return reg_read(0x0740, &sync_hi) && reg_read(0x0741, &sync_lo) &&
        sync_hi == (uint8_t)(SYNC_WORD >> 8) && sync_lo == (uint8_t)SYNC_WORD;
}

bool rf_init(void) {
    rf_abort(); rf_delay_us(2000);
    rf_reset(true); rf_delay_us(20000);
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
        CMD(0x8c, 0, PREAMBLE_SYMBOLS, 0, 255, 1, 0) && // Explicit header, CRC, normal IQ
        CMD(0x95, 4, 7, 0, 1) && // SX1262 PA configuration
        CMD(0x8e, (uint8_t)TX_POWER_DBM, 4) && // 200 us ramp
        CMD(0x93, 0x20) &&    // Fall back to STDBY_RC after TX.
        CMD(0x08, 0x02, 0x63, 0x02, 0x63, 0, 0, 0, 0) && // TX_DONE / TIMEOUT
        CMD(0x0d, 0x07, 0x40, (uint8_t)(SYNC_WORD >> 8), (uint8_t)SYNC_WORD);
    uint8_t clamp = 0, modulation = 0;
    // Semtech SX1262 PA clamp and 500 kHz LoRa modulation-quality workarounds.
    ok = ok && reg_read(0x08d8, &clamp) && reg_write(0x08d8, clamp | 0x1e) &&
        reg_read(0x0889, &modulation) && reg_write(0x0889, (uint8_t)(modulation & ~0x04u)) &&
        reg_write(0x08ac,0x96) && clear_irq() && rf_health(false);
    if (!ok) rf_abort();
    return ok;
}


uint64_t rf_airtime_us(size_t bytes) {
    /* SF12/BW500/CR4/8/LDRO=0/explicit header/CRC/preamble8. */
    unsigned groups=(unsigned)((8*bytes+43)/48);
    return (UINT64_C(81)+32*groups)*2048;
}
bool rf_idle(void) { return CMD(0x80,0) && clear_irq(); }
bool rf_receive(void) {
    return CMD(0x80,0) && clear_irq() && CMD(0x8c,0,PREAMBLE_SYMBOLS,0,255,1,0) && CMD(0x82,255,255,255);
}
bool rf_transmit_before(const uint8_t *p,size_t n,uint64_t start_deadline_us) {
    if(!n || n>255)return false;
    uint8_t buffer[257]={0x0e,0};memcpy(buffer+2,p,n);
    const uint32_t ticks=TX_TIMEOUT_MS*64u;
    return CMD(0x80,0) && clear_irq() &&
        CMD(0x8c,0,PREAMBLE_SYMBOLS,0,(uint8_t)n,1,0) &&
        command(buffer,n+2,0) && wait_ready() && rf_time_us()<start_deadline_us && CMD(0x83,(uint8_t)(ticks>>16),(uint8_t)(ticks>>8),(uint8_t)ticks);
}
int rf_tx_poll(void) {
    uint16_t flags=0;if(!irq_read(&flags))return -1;
    if(flags&0x200)return -1;
    if(!(flags&1))return 0;
    return CMD(0x80,0) && clear_irq() ? 1:-1;
}
int rf_rx_poll(uint8_t *p,size_t *n) {
    uint16_t flags=0;if(!irq_read(&flags))return -1;
    if(!flags)return 0;
    if(flags&0x200)return -1;
    if(!(flags&2) || (flags&0x60))return clear_irq() ? -2:-1;
    const uint8_t op=0x13;
    if(!command(&op,1,3))return -1;
    *n=rx[2];const uint8_t read[]={0x1e,rx[3]};
    if(!command(read,2,*n+1))return -1;
    memcpy(p,rx+3,*n);
    return clear_irq() ? 1:-1;
}

bool rf_transmit(const uint8_t *p,size_t n) {return rf_transmit_before(p,n,UINT64_MAX);}
