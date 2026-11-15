#include <initializer_list>
#include <stdio.h>
#include <inttypes.h>
#include "control.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP32-S3-Relay-6CH Pico header mapping, NOT Pico GP numbers.
static constexpr int CS=14, BUSY=13, RESET=12, SCK=35, MOSI=36, MISO=37;
static constexpr uint64_t IDLE_REFRESH_MS=3600000; // Refresh once/hour only with relay OFF.
static uint64_t ms() { return static_cast<uint64_t>(esp_timer_get_time()/1000); }
static void pause_ms(unsigned n) { vTaskDelay(pdMS_TO_TICKS(n)); }
static void set(int p,int v) { gpio_set_level(static_cast<gpio_num_t>(p),v); }
static void out(int p,int v) { set(p,v); gpio_set_direction(static_cast<gpio_num_t>(p),GPIO_MODE_OUTPUT); }
static int get(int p) { return gpio_get_level(static_cast<gpio_num_t>(p)); }

static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static bool indicated=false;
static void outputs(bool on) {
    set(1,on);
    if(on==indicated) return;
    rmt_symbol_word_t symbols[25]={};
    const uint32_t color=on?0x180000:0; // GRB: green
    for(int i=0;i<24;i++) {
        bool bit=color & (1u<<(23-i));
        symbols[i].level0=1; symbols[i].duration0=bit?8:4;
        symbols[i].level1=0; symbols[i].duration1=bit?4:8;
    }
    symbols[24].duration0=500; symbols[24].duration1=500;
    rmt_transmit_config_t cfg={};
    esp_err_t err=rmt_transmit(led_channel,led_encoder,symbols,sizeof(symbols),&cfg);
    if(err==ESP_OK) err=rmt_tx_wait_all_done(led_channel,50);
    if(err!=ESP_OK) { set(1,0); esp_restart(); }
    indicated=on;
    printf("RELAY %d t=%" PRIu64 "\n",on,ms());
}
static void led_init() {
    rmt_tx_channel_config_t cfg={}; cfg.gpio_num=GPIO_NUM_38;
    cfg.clk_src=RMT_CLK_SRC_DEFAULT; cfg.resolution_hz=10000000;
    cfg.mem_block_symbols=64; cfg.trans_queue_depth=1;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg,&led_channel));
    rmt_copy_encoder_config_t enc={};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc,&led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));
    indicated=true; outputs(false);
}

static spi_device_handle_t spi;
static uint8_t txbuf[32],rxbuf[32];
static bool command(const uint8_t *p,size_t n,size_t read=0) {
    if(n>sizeof(txbuf) || read>sizeof(txbuf)-n) return false;
    const uint64_t start=ms();
    while(get(BUSY)) { if(ms()-start>=BUSY_TIMEOUT_US/1000) return false; pause_ms(1); }
    memset(txbuf,0,n+read); memcpy(txbuf,p,n); memset(rxbuf,0,n+read);
    spi_transaction_t t={}; t.length=(n+read)*8; t.tx_buffer=txbuf; t.rx_buffer=rxbuf;
    set(CS,0); const esp_err_t e=spi_device_polling_transmit(spi,&t); set(CS,1);
    return e==ESP_OK;
}
template<size_t N> static bool cmd(const uint8_t (&p)[N]) { return command(p,N); }
static bool clear_irq() { const uint8_t p[]={2,0xff,0xff};return cmd(p); }
static bool receive() { const uint8_t p[]={0x82,255,255,255}; return cmd(p); }
static bool irq(uint16_t &value) {
    uint8_t p=0x12; if(!command(&p,1,3)) return false;
    value=(rxbuf[2]<<8)|rxbuf[3];return true;
}
static bool radio_healthy() {
    uint8_t p=0xc0;
    if(!command(&p,1,1) || (rxbuf[1]&0x70)!=0x50) return false;
    const unsigned status=(rxbuf[1]>>1)&7;
    if(status==3 || status==4 || status==5) return false;
    p=0x17;
    return command(&p,1,3) && rxbuf[2]==0 && rxbuf[3]==0;
}
static bool radio_init() {
    set(RESET,0);pause_ms(2);set(RESET,1);pause_ms(20);
    const uint32_t f=static_cast<uint32_t>((static_cast<uint64_t>(RF_FREQUENCY_HZ)<<25)/32000000);
    const uint32_t tcxo=TCXO_STARTUP_MS*64u;
    const uint8_t init[][9]={
        {0x80,0},{0x97,TCXO_VOLTAGE_CODE,static_cast<uint8_t>(tcxo>>16),static_cast<uint8_t>(tcxo>>8),static_cast<uint8_t>(tcxo)},
        {0x9d,1},{7,0,0},{0x89,0x7f},{0x8a,1},{0x98,0xe1,0xe9},
        {0x86,static_cast<uint8_t>(f>>24),static_cast<uint8_t>(f>>16),static_cast<uint8_t>(f>>8),static_cast<uint8_t>(f)},
        {0x8b,LORA_SF,LORA_BW_CODE,LORA_CR_CODE,LORA_LDRO},
        {0x8f,0,0},{0x8c,0,PREAMBLE_SYMBOLS,0,255,1,0},
        {0x0d,0x08,0xac,0x96}, // Boosted RX gain.
        {0x0d,0x07,0x40,static_cast<uint8_t>(SYNC_WORD>>8),static_cast<uint8_t>(SYNC_WORD)},
        {8,2,0x62,2,0x62,0,0,0,0}
    };
    const size_t sizes[]={2,5,2,3,2,2,3,5,5,3,7,4,5,9};
    for(size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) if(!command(init[i],sizes[i])) return false;
    return clear_irq() && receive() && radio_healthy();
}

// 0: no usable packet, 1: packet, -1: radio fault. CRC/header failures discarded.
static int poll(uint8_t *packet,size_t &length) {
    uint16_t flags;if(!irq(flags)) return -1;
    if(!flags) return 0;
    if(flags&0x200) return -1;
    int result=0;
    if((flags&2) && !(flags&0x60)) {
        uint8_t p=0x13;if(!command(&p,1,3)) return -1;
        length=rxbuf[2];uint8_t read[]={0x1e,rxbuf[3]};
        if(length==ON_PACKET_SIZE) {
            if(!command(read,2,length+1)) return -1;
            memcpy(packet,rxbuf+3,length);result=1;
        }
    }
    if(!clear_irq()||!receive()) return -1;
    return result;
}

extern "C" void app_main() {
    for(int p:{1,2,41,42,45,46}) out(p,0); // All relays OFF before peripheral initialization.
    // Subscribe before any peripheral initialization can block while yielding.
    esp_task_wdt_config_t wd={};wd.timeout_ms=WATCHDOG_MS;wd.idle_core_mask=3;wd.trigger_panic=true;
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wd));ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    led_init();
    out(CS,1);out(RESET,0);gpio_set_direction(static_cast<gpio_num_t>(BUSY),GPIO_MODE_INPUT);
    spi_bus_config_t bus={};bus.mosi_io_num=MOSI;bus.miso_io_num=MISO;
    bus.sclk_io_num=SCK;bus.quadwp_io_num=-1;bus.quadhd_io_num=-1;bus.max_transfer_sz=32;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_DISABLED));
    spi_device_interface_config_t dev={};dev.clock_speed_hz=SPI_BAUD_HZ;dev.spics_io_num=-1;dev.queue_size=1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST,&dev,&spi));
    Control control;bool ready=false;unsigned failures=0;
    uint64_t next_try=0,last_refresh=0,last_health=0,last_log=0,accepted=0,rejected=0;
    printf("BOOT " FIRMWARE_VERSION " receiver ON-only SF=%u hold=%" PRIu64 " reset=%d\n",LORA_SF,RECEIVER_HOLD_MS,esp_reset_reason());
    for(;;) {
        uint64_t now=ms();control.tick(now);outputs(control.on);
        if(!ready && now>=next_try) {
            ready=radio_init();next_try=ms()+RADIO_RETRY_MS;
            if(ready) {failures=0;last_refresh=ms();last_health=ms();printf("RADIO READY\n");}
            else if(++failures>=MAX_INIT_FAILURES) esp_restart();
        }
        if(ready) {
            uint8_t packet[ON_PACKET_SIZE];size_t n=0;const int result=poll(packet,n);
            if(result<0) ready=false;
            if(result==1) {
                if(control.accept(packet,n,ms())) {
                    ++accepted;outputs(control.on);
                    printf("RX ON t=%" PRIu64 "\n",ms());
                } else ++rejected; // Includes legacy OFF: cannot turn on or renew.
            }
            now=ms();
            if(now-last_health>=RADIO_HEALTH_MS) {
                ready=ready&&radio_healthy();last_health=ms();
            }
            if(ready && !control.on && now-last_refresh>=IDLE_REFRESH_MS) {
                ready=false;next_try=ms();printf("RADIO scheduled idle refresh\n");
            }
        }
        if(!ready) {control.fail();outputs(false);set(RESET,0);}
        control.tick(ms());outputs(control.on); // Recheck lease after bounded radio operations.
        now=ms();
        if(now-last_log>=HEALTH_LOG_MS) {
            printf("HEALTH ready=%d on=%d accepted=%" PRIu64 " rejected=%" PRIu64
                   " free=%u min=%u stack=%u\n",ready,control.on,accepted,rejected,
                   static_cast<unsigned>(esp_get_free_heap_size()),static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                   static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            last_log=now;
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());pause_ms(10);
    }
}
