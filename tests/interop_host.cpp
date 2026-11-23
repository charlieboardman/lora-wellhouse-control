/* Host adapter: real gateway state machines plus a deterministic RTU bus.
   Python supplies TCP bytes over stdio; no radio or serial hardware is used. */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "../firmware/gateway/frontend.h"
#include "../firmware/gateway/sender.c"
#include "../firmware/gateway/receiver.c"
static uint8_t response[260];static size_t response_n;static uint64_t now;
static uint16_t registers[2][256];
static gw_sender sender;static gw_receiver receiver;
void gw_deliver(uint32_t,uint16_t id,const uint8_t *p,size_t n,uint8_t error) {
    size_t length=error?3:n;response[0]=(uint8_t)(id>>8);response[1]=(uint8_t)id;
    response[2]=response[3]=0;response[4]=(uint8_t)(length>>8);response[5]=(uint8_t)length;
    if(error){response[6]=p[0];response[7]=p[1]|0x80;response[8]=error;}
    else memcpy(response+6,p,n);
    response_n=length+6;
}
void gw_control_done(uint32_t,bool){}
static void serial(gw_packet *out) {
    const uint8_t *p=receiver.request.data;size_t request_n=receiver.request.length;
    uint8_t frame[256]={p[0],p[1]};size_t n=0;uint8_t error=0;
    if(p[0]>2){gw_receiver_serial_done(&receiver,frame,0,now,out);return;}
    unsigned address=request_n>=4?(p[2]<<8)|p[3]:0;
    unsigned count=request_n>=6?(p[4]<<8)|p[5]:0;
    uint16_t *bank=registers[p[0]-1];
    if(p[1]==3 || p[1]==4) {
        if(request_n!=6 || !count || count>125)error=3;
        else if(address+count>256)error=2;
        else {frame[2]=(uint8_t)(count*2);n=3;
            for(unsigned i=0;i<count;i++){frame[n++]=(uint8_t)(bank[address+i]>>8);frame[n++]=(uint8_t)bank[address+i];}}
    } else if(p[1]==6) {
        if(request_n!=6)error=3;
        else if(address>=256)error=2;
        else {bank[address]=(uint16_t)count;memcpy(frame,p,6);n=6;}
    } else if(p[1]==16) {
        if(request_n<7 || !count || request_n!=7+2*count || p[6]!=2*count)error=3;
        else if(address+count>256)error=2;
        else {for(unsigned i=0;i<count;i++)bank[address+i]=(uint16_t)((p[7+2*i]<<8)|p[8+2*i]);memcpy(frame,p,6);n=6;}
    } else if(p[1]==0x41){n=254;for(size_t i=2;i<n;i++)frame[i]=(uint8_t)i;}
    else error=1;
    if(error){frame[1]=p[1]|0x80;frame[2]=error;n=3;}
    uint16_t crc=gw_crc(frame,n);frame[n++]=(uint8_t)crc;frame[n++]=(uint8_t)(crc>>8);
    gw_receiver_serial_done(&receiver,frame,n,now,out);
}
static void step(gw_packet p) {
    uint8_t wire[255];gw_packet decoded={},out={};size_t n=gw_encode(&p,wire);
    assert(n && gw_decode(&decoded,wire,n));int action=gw_receiver_request(&receiver,&decoded,++now,&out);
    if(action==2)serial(&out);else assert(action==1);
    n=gw_encode(&out,wire);assert(n && gw_decode(&decoded,wire,n));assert(gw_sender_reply(&sender,&decoded,++now));
}
int main() {
    for(unsigned unit=0;unit<2;unit++)for(unsigned i=0;i<256;i++)registers[unit][i]=(uint16_t)((unit+1)*1000+i);
    gw_sender_init(&sender,123,0);gw_receiver_init(&receiver,456);gw_packet p;
    assert(gw_sender_next(&sender,0,&p));step(p);
    char line[2048];
    while(fgets(line,sizeof(line),stdin)) {
        uint8_t input[260];size_t used=0;char *cursor=line;
        while(cursor[0] && cursor[0]!='\n' && cursor[1] && used<sizeof(input)) {
            char hex[3]={cursor[0],cursor[1],0};input[used++]=(uint8_t)strtoul(hex,NULL,16);cursor+=2;
        }
        size_t frame_size=0;response_n=0;
        if(gw_mbap(input,used,&frame_size)!=1 || frame_size!=used)return 2;
        uint16_t id=(uint16_t)((input[0]<<8)|input[1]);
        uint8_t error=gw_sender_submit(&sender,1,id,input+6,used-6,now/1000);
        if(error)gw_deliver(1,id,input+6,2,error);
        else for(unsigned i=0;!response_n && i<8;i++){assert(gw_sender_next(&sender,now/1000,&p));step(p);}
        assert(response_n);
        for(size_t i=0;i<response_n;i++)printf("%02x",response[i]);
        puts("");fflush(stdout);
    }
}
