#pragma once
#include <stdint.h>

// User settings. GP numbers, not physical header pin numbers.
#define FIRMWARE_VERSION "1.1.1"
#define FLOAT_GPIO 0u                 // GP0 / physical pin 1; switch to GND (pin 3).
#define SAMPLE_INTERVAL_MS UINT64_C(10000)
#define RECEIVER_HOLD_MS UINT64_C(31000) // Documentation/check only: configured in receiver.
#define TX_POWER_DBM 22               // HAT maximum: nominal 158 mW conducted RF output.
#define RF_FREQUENCY_HZ UINT32_C(915000000)
#define LORA_SF 12u
#define LORA_BW_CODE 4u                // 125 kHz
#define LORA_CR_CODE 4u                // 4/8
#define LORA_LDRO 1u
#define PREAMBLE_SYMBOLS 8u
#define SYNC_WORD 0x1424u              // Private LoRa sync word; matches receiver reset default.

#define WATCHDOG_MS 8000u
#define RADIO_RETRY_MS UINT64_C(2000)
#define RADIO_HEALTH_MS UINT64_C(1000)
#define SENDER_RADIO_REFRESH_MS UINT64_C(3600000) // Reconfigure after a completed hourly sample/TX.
#define HEALTH_LOG_MS UINT64_C(60000)
#define BUSY_TIMEOUT_US UINT64_C(250000)
#define TX_TIMEOUT_MS 2500u
#define TX_WAIT_MS UINT64_C(2700)
#define MAX_INIT_FAILURES 8u

// Waveshare Pico-LoRa-SX1262 HAT: SPI1, DIO2 RF switch, DIO3 1.7V TCXO.
#define RADIO_SCK 10u
#define RADIO_MOSI 11u
#define RADIO_MISO 12u
#define RADIO_CS 3u
#define RADIO_BUSY 2u
#define RADIO_RESET 15u
#define RADIO_DIO1 20u
#define TCXO_VOLTAGE_CODE 1u           // 1.7V per Waveshare example.
#define TCXO_STARTUP_MS 8u
#define SPI_BAUD_HZ 1000000u

// Existing receiver identifier, retained for compatibility; not authentication.
#define ON_PACKET_BYTES 'L','R','1',0x00,0x01,0x02,0x03,0x04,0x05,1
#define ON_PACKET_SIZE 10u

#if FLOAT_GPIO == RADIO_SCK || FLOAT_GPIO == RADIO_MOSI || FLOAT_GPIO == RADIO_MISO || \
    FLOAT_GPIO == RADIO_CS || FLOAT_GPIO == RADIO_BUSY || FLOAT_GPIO == RADIO_RESET || \
    FLOAT_GPIO == RADIO_DIO1 || FLOAT_GPIO == 26u || FLOAT_GPIO > 28u || \
    FLOAT_GPIO == 23u || FLOAT_GPIO == 24u || FLOAT_GPIO == 25u
#error "Float GPIO conflicts with radio, battery ADC, or Pico W internal connections"
#endif
#if SAMPLE_INTERVAL_MS <= TX_WAIT_MS || RECEIVER_HOLD_MS <= 3 * SAMPLE_INTERVAL_MS
#error "Keep TX shorter than the interval and receiver hold longer than three intervals"
#endif
#if SENDER_RADIO_REFRESH_MS < SAMPLE_INTERVAL_MS
#error "Radio refresh interval must be at least one sample interval"
#endif
#if TX_POWER_DBM < -9 || TX_POWER_DBM > 22
#error "SX1262 power must be -9 to +22 dBm"
#endif
#if LORA_SF != 12 || LORA_BW_CODE != 4 || LORA_CR_CODE != 4 || LORA_LDRO != 1
#error "Radio changes require matching receiver configuration and airtime/timeout review"
#endif
#if RF_FREQUENCY_HZ < 902000000 || RF_FREQUENCY_HZ > 928000000
#error "Image calibration in this driver is for the 902-928 MHz band"
#endif
