#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../config.h"

/* Explicit byte encoding: independent of CPU alignment and byte order. */
#define GW_VERSION 1u
#define GW_HEADER_SIZE 32u
#define GW_PACKET_MAX 255u
#define GW_DATA_MAX (GW_PACKET_MAX-GW_HEADER_SIZE)
#define GW_RESPONSE_MAX 256u
#define GW_QUEUE_SIZE 8u

enum gw_kind { GW_HELLO=1, GW_REQUEST, GW_RESULT, GW_READY, GW_FETCH,
    GW_PIECE, GW_ON, GW_PAUSE, GW_RESUME, GW_SYNC, GW_CORRECTION, GW_ERROR };
typedef struct {
    uint8_t kind, piece;
    uint64_t session, receiver_boot;
    uint32_t transaction;
    uint8_t data[GW_DATA_MAX];
    size_t length;
} gw_packet;
static inline void gw_put64(uint8_t *p,uint64_t v) {
    for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i));
}
static inline uint64_t gw_get64(const uint8_t *p) {
    uint64_t v=0;for(unsigned i=0;i<8;i++) v=(v<<8)|p[i];return v;
}
static inline size_t gw_encode(const gw_packet *p,uint8_t out[GW_PACKET_MAX]) {
    if(p->length>GW_DATA_MAX || p->kind<GW_HELLO || p->kind>GW_ERROR)return 0;
    out[0]='W';out[1]=GW_VERSION;out[2]=p->kind;out[3]=p->piece;
    gw_put64(out+4,p->session);gw_put64(out+12,p->receiver_boot);
    for(unsigned i=0;i<4;i++)out[20+i]=(uint8_t)(p->transaction>>(24-8*i));
    gw_put64(out+24,GW_LINK_ID);
    memcpy(out+GW_HEADER_SIZE,p->data,p->length);return GW_HEADER_SIZE+p->length;
}
static inline bool gw_decode(gw_packet *p,const uint8_t *in,size_t n) {
    if(n<GW_HEADER_SIZE || n>GW_PACKET_MAX || in[0]!='W' || in[1]!=GW_VERSION ||
       in[2]<GW_HELLO || in[2]>GW_ERROR || gw_get64(in+24)!=GW_LINK_ID)return false;
    p->kind=in[2];p->piece=in[3];p->session=gw_get64(in+4);p->receiver_boot=gw_get64(in+12);
    p->transaction=0;for(unsigned i=0;i<4;i++)p->transaction=(p->transaction<<8)|in[20+i];
    p->length=n-GW_HEADER_SIZE;memcpy(p->data,in+GW_HEADER_SIZE,p->length);return true;
}
static inline uint16_t gw_crc(const uint8_t *p,size_t n) {
    uint16_t crc=0xffff;
    for(size_t i=0;i<n;i++) {crc^=p[i];for(unsigned b=0;b<8;b++)
        crc=(uint16_t)((crc>>1)^((crc&1)?0xa001:0));}
    return crc;
}
/* One response, retained across ON/control/sync traffic. Duplicate identity must
   include original bytes; a reused ID with different content is not a retry. */
typedef struct {
    bool valid;
    uint64_t session, acquired_us;
    uint32_t transaction;
    uint8_t request[GW_DATA_MAX], response[GW_RESPONSE_MAX];
    size_t request_length,response_length;
} gw_cache;
static inline bool gw_duplicate(const gw_cache *c,const gw_packet *p) {
    return c->valid && c->session==p->session && c->transaction==p->transaction &&
        c->request_length==p->length && memcmp(c->request,p->data,p->length)==0;
}
static inline size_t gw_piece(const gw_cache *c,unsigned piece,uint8_t *out) {
    if(!c->valid || c->response_length>GW_RESPONSE_MAX)return 0;
    size_t offset=(size_t)piece*GW_DATA_MAX;
    if(offset>=c->response_length)return 0;
    size_t n=c->response_length-offset;if(n>GW_DATA_MAX)n=GW_DATA_MAX;
    memcpy(out,c->response+offset,n);return n;
}

static inline bool gw_rtu_matches(const uint8_t *request,size_t request_n,const uint8_t *frame,size_t n) {
    if(request_n<2 || n<5 || n>GW_RESPONSE_MAX || frame[0]!=request[0] || gw_crc(frame,n)!=0)return false;
    if(frame[1]==(uint8_t)(request[1]|0x80))return n==5;
    if(frame[1]!=request[1])return false;
    unsigned function=request[1];
    if(function>=1 && function<=4) {
        if(request_n!=6)return false;
        size_t count=((size_t)request[4]<<8)|request[5];
        size_t bytes=function<=2?(count+7)/8:count*2;
        return count && n==bytes+5 && frame[2]==bytes;
    }
    if(function==5 || function==6 || function==15 || function==16)
        return request_n>=6 && n==8 && memcmp(request,frame,6)==0;
    if(function==22)return request_n==8 && n==10 && memcmp(request,frame,8)==0;
    return true;
}
