#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Board HAL. Transactions are synchronous SPI only; RF completion is polled. */
bool rf_transfer(const uint8_t *tx,uint8_t *rx,size_t n);
bool rf_busy(void);
void rf_reset(bool high);
void rf_delay_us(unsigned us);
uint64_t rf_time_us(void);
bool rf_init(void);
void rf_abort(void);
bool rf_health(bool receiving);
bool rf_idle(void);
bool rf_receive(void);
bool rf_transmit(const uint8_t *p,size_t n);
bool rf_transmit_before(const uint8_t *p,size_t n,uint64_t start_deadline_us);
/* 1=complete, 0=pending, -1=fault. RX: -2=discarded CRC/header error. */
int rf_tx_poll(void);
int rf_rx_poll(uint8_t *p,size_t *n);
uint64_t rf_airtime_us(size_t bytes);
#ifdef __cplusplus
}
#endif
