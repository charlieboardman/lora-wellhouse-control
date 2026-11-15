#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../firmware/receiver/control.h"
#include "../firmware/sender/sender_logic.h"

int main() {
    constexpr uint8_t packet[]={ON_PACKET_BYTES};
    static_assert(sizeof(packet)==ON_PACKET_SIZE);
    // 32-bit timer boundaries and >20 years uptime, using the actual shared logic.
    for(uint64_t base : {0ULL,2147483640ULL,4294967290ULL,631152000000ULL}) {
        sender_schedule_t s; sender_schedule_init(&s,base);
        assert(!sender_take_slot(&s,base));
        assert(!sender_take_slot(&s,base+9999));
        assert(sender_take_slot(&s,base+10000));
        assert(!sender_take_slot(&s,base+10001));
        assert(sender_take_slot(&s,base+70000)); // Stalled CPU: one sample, no catch-up burst.
        assert(!sender_take_slot(&s,base+70000));
        assert(!sender_take_slot(&s,base+79999));
        assert(sender_take_slot(&s,base+80000));
        Control c;
        assert(!c.on);
        assert(c.accept(packet,sizeof(packet),base));
        c.tick(base+30999);assert(c.on);
        c.tick(base+31000);assert(!c.on);
        assert(c.accept(packet,sizeof(packet),base));
        assert(c.accept(packet,sizeof(packet),base+30000)); // Two lost 10s heartbeats tolerated.
        c.tick(base+31000);assert(c.on);
        const uint64_t deadline=c.deadline;
        uint8_t bad[ON_PACKET_SIZE+1]={};memcpy(bad,packet,sizeof(packet));
        for(size_t i=0;i<sizeof(packet);++i) {
            bad[i]^=1; // Includes legacy OFF byte 0.
            assert(!c.accept(bad,sizeof(packet),base+60000));
            assert(c.deadline==deadline);
            bad[i]^=1;
        }
        for(size_t n=0;n<sizeof(packet);++n) assert(!c.accept(packet,n,base+60000));
        assert(!c.accept(bad,sizeof(bad),base+60000));
        bad[sizeof(packet)-1]=2;assert(!c.accept(bad,sizeof(packet),base+60000));
        c.tick(base+61000);assert(!c.on);
        bad[sizeof(packet)-1]=0;assert(!c.accept(bad,sizeof(packet),base+62000));assert(!c.on);
        c.accept(packet,sizeof(packet),base+70000);c.fail();assert(!c.on);
    }
    assert(!sender_should_transmit(false,false));
    assert(!sender_should_transmit(false,true));
    assert(!sender_should_transmit(true,false));
    assert(sender_should_transmit(true,true));

    // Integrated timing simulation: uninterrupted ON, then silence with no OFF frames.
    sender_schedule_t schedule;sender_schedule_init(&schedule,0);
    Control receiver;
    unsigned sends=0;
    for(uint64_t t=0;t<=100000;t++) {
        if(sender_take_slot(&schedule,t) && sender_should_transmit(t<=60000,true)) {
            assert(receiver.accept(packet,sizeof(packet),t));++sends;
        }
        receiver.tick(t);
        assert(receiver.on==(t>=10000 && t<91000));
    }
    assert(sends==6);
    puts("PASS scheduled ON-only TX, silence timeout, renewal, rejected OFF/invalid frames, fault-off, 20-year timers");
}
