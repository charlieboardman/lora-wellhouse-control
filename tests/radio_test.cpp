#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../firmware/gateway/radio.h"
#include "../firmware/config.h"
static uint64_t now_us;static bool busy;static uint16_t irq_flags;
static uint8_t fifo[255];static size_t fifo_n;static std::vector<uint8_t> last_buffer;
bool rf_transfer(const uint8_t *tx,uint8_t *rx,size_t n) {
    memset(rx,0,n);
    if(tx[0]==0x12){rx[2]=(uint8_t)(irq_flags>>8);rx[3]=(uint8_t)irq_flags;}
    if(tx[0]==0x02)irq_flags=0;
    if(tx[0]==0x13){rx[2]=(uint8_t)fifo_n;rx[3]=0;}
    if(tx[0]==0x1e)memcpy(rx+3,fifo,fifo_n);
    if(tx[0]==0x0e)last_buffer.assign(tx+2,tx+n);
    return true;
}
bool rf_busy(void){return busy;}
void rf_reset(bool){}
void rf_delay_us(unsigned us){now_us+=us;}
uint64_t rf_time_us(void){return now_us;}
int main() {
    assert(rf_airtime_us(10)==296960);assert(rf_airtime_us(255)==2983936);
    for(size_t i=0;i<255;i++)fifo[i]=(uint8_t)i;
    assert(!rf_transmit(fifo,256));assert(!rf_transmit(fifo,0));
    assert(rf_transmit(fifo,255));assert(last_buffer.size()==255);
    for(unsigned i=0;i<100;i++)assert(rf_tx_poll()==0); // no RF busy-wait loop
    irq_flags=1;assert(rf_tx_poll()==1);
    assert(rf_receive());uint8_t output[255];size_t n=0;fifo_n=255;irq_flags=2;
    assert(rf_rx_poll(output,&n)==1 && n==255 && !memcmp(output,fifo,255));
    irq_flags=0x42;assert(rf_rx_poll(output,&n)==-2); // CRC error discarded
    busy=true;assert(!rf_receive());assert(now_us>=BUSY_TIMEOUT_US);
    puts("PASS actual SX1262 driver: full packets, airtime, nonblocking RF polls, CRC discard, BUSY deadline");
}

#include "../firmware/gateway/radio.c"
