#include <cassert>
#include <cstdio>
#include "../firmware/gateway/protocol.h"
#include "../firmware/gateway/frontend.h"
int main() {
    gw_packet p={},q={};uint8_t wire[GW_PACKET_MAX]={};
    p.kind=GW_REQUEST;p.session=UINT64_C(0x0123456789abcdef);
    p.receiver_boot=UINT64_C(0xfedcba9876543210);p.transaction=0x12345678;
    p.length=GW_DATA_MAX;
    for(size_t i=0;i<p.length;i++)p.data[i]=(uint8_t)i;
    assert(gw_encode(&p,wire)==255);assert(gw_decode(&q,wire,255));
    assert(q.session==p.session && q.receiver_boot==p.receiver_boot);
    assert(q.transaction==p.transaction && q.length==p.length);
    assert(memcmp(q.data,p.data,p.length)==0);
    assert(!gw_decode(&q,wire,23));assert(!gw_decode(&q,wire,256));
    p.length++;assert(gw_encode(&p,wire)==0);p.length--;
    const uint8_t check[]={1,3,0,0,0,1};assert(gw_crc(check,6)==0x0a84);
    gw_cache c={};c.valid=true;c.session=p.session;c.transaction=p.transaction;
    c.request_length=p.length;memcpy(c.request,p.data,p.length);
    assert(gw_duplicate(&c,&p));p.data[0]^=1;assert(!gw_duplicate(&c,&p));
    c.response_length=256;
    for(size_t i=0;i<256;i++)c.response[i]=(uint8_t)i;
    uint8_t result[256]={};
    assert(gw_piece(&c,0,result)==GW_DATA_MAX);
    assert(gw_piece(&c,1,result+GW_DATA_MAX)==256-GW_DATA_MAX);
    assert(gw_piece(&c,2,wire)==0);assert(memcmp(result,c.response,256)==0);
    uint8_t tcp[]={0,1,0,0,0,6,1,3,0,0,0,1};size_t size=0;
    for(size_t n=0;n<sizeof(tcp);n++)assert(gw_mbap(tcp,n,&size)==0);
    assert(gw_mbap(tcp,sizeof(tcp),&size)==1 && size==sizeof(tcp));
    tcp[2]=1;assert(gw_mbap(tcp,sizeof(tcp),&size)==-1);tcp[2]=0;
    tcp[4]=1;tcp[5]=0;assert(gw_mbap(tcp,sizeof(tcp),&size)==-1);
    const char *http="POST /pump/pause HTTP/1.1\r\nHost: pico\r\nContent-Length: 0\r\n\r\n";
    assert(gw_http(http,strlen(http))==HTTP_PAUSE);
    http="GET / HTTP/1.1\r\n\r\n";assert(gw_http(http,strlen(http))==HTTP_INDEX);
    http="GET /pump/status HTTP/1.1\r\n\r\n";assert(gw_http(http,strlen(http))==HTTP_STATUS);
    http="POST /modbus/send?pdu=010321000001 HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    assert(gw_http(http,strlen(http))==HTTP_MODBUS);
    uint8_t pdu[16]={};assert(gw_http_modbus_pdu(http,strlen(http),pdu,sizeof(pdu))==6);
    const uint8_t exp_pdu[]={1,3,0x21,0,0,1};assert(memcmp(pdu,exp_pdu,6)==0);
    http="POST /modbus/send?pdu=010 HTTP/1.1\r\n\r\n";
    assert(gw_http(http,strlen(http))==HTTP_MODBUS);
    assert(gw_http_modbus_pdu(http,strlen(http),pdu,sizeof(pdu))==0);
    http="POST /modbus/send?pdu=01zz HTTP/1.1\r\n\r\n";
    assert(gw_http_modbus_pdu(http,strlen(http),pdu,sizeof(pdu))==0);
    http="POST /pump/resume HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";assert(gw_http(http,strlen(http))==HTTP_BAD);
    http="POST /pump/resume HTTP/1.1\r\nContent-Length: x\r\n\r\n";assert(gw_http(http,strlen(http))==HTTP_BAD);
    http="POST /pump/resume HTTP/1.1\r\nContent-Length: 0\r\n\r\nx";assert(gw_http(http,strlen(http))==HTTP_BAD);
    assert(gw_http("GET /pump/status HTTP/1.1",24)==HTTP_INCOMPLETE);
    const uint64_t day=UINT64_C(86400000000);
    assert(gw_daily_sync_due(false,0,0));assert(!gw_daily_sync_due(true,day*10+1,10));
    assert(gw_daily_sync_due(true,day*11,10));assert(!gw_daily_sync_due(true,day*9,10));
    puts("PASS gateway framing, request bounds, CRC, duplicate identity, response pieces");
}
