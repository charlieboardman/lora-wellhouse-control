// Execute the actual sender loop with a simulated clock/radio. No test hooks
// are compiled into the production firmware.
#include <cassert>
#include <cstdio>
#include <vector>
#include "../firmware/receiver/control.h"
static int test_printf(const char *, ...);
#define printf test_printf
#define main sender_firmware_main
#include "../firmware/sender/main.c"
#undef main
#undef printf

struct End { bool reboot; };
static uint64_t clock_ms, base_ms, fed_ms, last_tx_end;
static bool permanent_failure, failed_refresh, enabled;
static unsigned init_calls, successes, tx_count;
static std::vector<uint64_t> samples, refreshes;
static Control receiver;
static constexpr uint8_t packet[] = {ON_PACKET_BYTES};

static void advance(unsigned n) {
    clock_ms += n;
    receiver.tick(clock_ms);
    assert(enabled && clock_ms-fed_ms < WATCHDOG_MS);
    // A failed refresh must not interrupt an otherwise continuous ON lease.
    if (!permanent_failure && tx_count && clock_ms-base_ms < SENDER_RADIO_REFRESH_MS+20000)
        assert(receiver.on);
}
uint64_t time_us_64(void) { return clock_ms*1000; }
void gpio_init(unsigned pin) { assert(pin==FLOAT_GPIO); }
void gpio_set_dir(unsigned, bool output) { assert(!output); }
void gpio_pull_up(unsigned) {}
bool gpio_get(unsigned pin) {
    assert(pin==FLOAT_GPIO);
    samples.push_back(clock_ms);
    // First maintenance is during ON; the next is during silence.
    return clock_ms-base_ms >= SENDER_RADIO_REFRESH_MS+20000;
}
void stdio_init_all(void) {}
void sleep_ms(unsigned n) {
    advance(n);
    if(clock_ms-base_ms >= 2*SENDER_RADIO_REFRESH_MS+60000) throw End{false};
}
void tight_loop_contents(void) { assert(false); }
int cyw43_arch_init(void) { return 0; }
void cyw43_arch_gpio_put(unsigned, bool) {}
void watchdog_enable(unsigned timeout, bool pause) {
    assert(timeout==8000 && !pause); enabled=true;fed_ms=clock_ms;
}
bool watchdog_caused_reboot(void) { return false; }
void watchdog_update(void) { fed_ms=clock_ms; }
void watchdog_reboot(unsigned pc, unsigned sp, unsigned delay) {
    assert(!pc && !sp && delay==10); throw End{true};
}
void radio_gpio_init(void) {}
void radio_abort(void) {}
bool radio_healthy(void) { return true; }
bool radio_init(void) {
    ++init_calls;
    if(!permanent_failure && init_calls>1) {
        assert(!samples.empty());
        assert(clock_ms>=last_tx_end); // Never reset during a packet.
        refreshes.push_back(clock_ms);
    }
    advance(75); // Initialization consumes time; scheduling must tolerate it.
    if(permanent_failure) return false;
    if(init_calls==2) {failed_refresh=true;return false;}
    ++successes;return true;
}
bool radio_send_on(void) {
    assert(!samples.empty() && clock_ms==samples.back());
    advance(1200);
    last_tx_end=clock_ms;
    assert(receiver.accept(packet,sizeof(packet),clock_ms));
    ++tx_count;return true;
}
static int test_printf(const char *, ...) { return 0; }

static void run(uint64_t base, bool fail) {
    clock_ms=base_ms=fed_ms=base;last_tx_end=0;
    permanent_failure=fail;failed_refresh=enabled=false;
    init_calls=successes=tx_count=0;receiver=Control{};
    samples.clear();refreshes.clear();led_available=led_state=false;
    try {sender_firmware_main();assert(false);}
    catch(const End &end) {assert(end.reboot==fail);}
    if(fail) {assert(init_calls==MAX_INIT_FAILURES && tx_count==0);return;}
    assert(failed_refresh && init_calls==4 && successes==3);
    assert(refreshes.size()==3);
    assert(refreshes[0]>=base+SENDER_RADIO_REFRESH_MS);
    assert(refreshes[1]-refreshes[0]>=RADIO_RETRY_MS+75);
    assert(refreshes[2]-refreshes[1]>=SENDER_RADIO_REFRESH_MS);
    assert(samples.front()==base+SAMPLE_INTERVAL_MS);
    for(size_t i=1;i<samples.size();i++)
        assert(samples[i]-samples[i-1]==SAMPLE_INTERVAL_MS);
    assert(tx_count==(SENDER_RADIO_REFRESH_MS+20000)/SAMPLE_INTERVAL_MS-1);
    assert(!receiver.on && !led_state);
}
int main() {
    static_assert(TX_POWER_DBM==22 && LORA_SF==12 && LORA_BW_CODE==4 && LORA_CR_CODE==4);
    run(0,false);
    run(UINT64_C(631152000000),false);
    run(0,true);
    puts("PASS actual sender loop: hourly refresh during ON/silence, failed-refresh recovery, unchanged 10s cadence, no extra TX, repeated-failure reboot, full-power settings");
}
