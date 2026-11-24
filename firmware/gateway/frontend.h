#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
/* TCP is a byte stream: -1 malformed, 0 incomplete, 1 complete. */
static inline int gw_mbap(const uint8_t *p,size_t n,size_t *frame_size) {
    if(n<6)return 0;
    size_t length=((size_t)p[4]<<8)|p[5];
    if(p[2] || p[3] || length<2 || length>254)return -1;
    *frame_size=length+6;return n>=*frame_size?1:0;
}
enum gw_http_route { HTTP_INCOMPLETE,HTTP_BAD,HTTP_UNKNOWN,HTTP_INDEX,HTTP_PAUSE,HTTP_RESUME,HTTP_STATUS,HTTP_RESULT,HTTP_MODBUS };
/* Caller supplies a terminated buffer; byte count excludes its terminator. */
static inline enum gw_http_route gw_http(const char *p,size_t n) {
    const char *end=strstr(p,"\r\n\r\n");if(!end)return HTTP_INCOMPLETE;
    const char *line_end=strstr(p,"\r\n");if(!line_end || line_end==p)return HTTP_BAD;
    const char *sp1=(const char *)memchr(p,' ',(size_t)(line_end-p));if(!sp1 || sp1==p)return HTTP_BAD;
    const char *path=sp1+1;
    const char *sp2=(const char *)memchr(path,' ',(size_t)(line_end-path));if(!sp2 || sp2==path)return HTTP_BAD;
    const char *ver=sp2+1;size_t ver_len=(size_t)(line_end-ver);
    if(ver_len!=8 || (memcmp(ver,"HTTP/1.1",8) && memcmp(ver,"HTTP/1.0",8)))return HTTP_BAD;
    for(const char *h=line_end+2;h<end;) {
        const char *next=strstr(h,"\r\n");if(!next)return HTTP_BAD;
        if(!strncasecmp(h,"Transfer-Encoding:",18))return HTTP_BAD;
        if(!strncasecmp(h,"Content-Length:",15)) {
            const char *v=h+15;while(v<next && (*v==' ' || *v=='\t'))++v;
            if(v==next)return HTTP_BAD;
            for(;v<next;v++)if(*v!='0' && *v!=' ' && *v!='\t')return HTTP_BAD;
        }
        h=next+2;
    }
    if(n!=(size_t)(end-p)+4)return HTTP_BAD;
    size_t m_len=(size_t)(sp1-p),p_len=(size_t)(sp2-path);
    if(m_len==3 && !memcmp(p,"GET",3)) {
        if(p_len==1 && path[0]=='/')return HTTP_INDEX;
        if(p_len==12 && !memcmp(path,"/pump/status",12))return HTTP_STATUS;
        if(p_len==19 && !memcmp(path,"/modbus/last-result",19))return HTTP_RESULT;
    }
    if(m_len==4 && !memcmp(p,"POST",4)) {
        if(p_len==11 && !memcmp(path,"/pump/pause",11))return HTTP_PAUSE;
        if(p_len==12 && !memcmp(path,"/pump/resume",12))return HTTP_RESUME;
        if(p_len>17 && !memcmp(path,"/modbus/send?pdu=",17))return HTTP_MODBUS;
    }
    return HTTP_UNKNOWN;
}
static inline int gw_hex_nibble(char c) {
    if(c>='0' && c<='9')return c-'0';
    if(c>='a' && c<='f')return c-'a'+10;
    if(c>='A' && c<='F')return c-'A'+10;
    return -1;
}
/* Decode hex PDU from a validated POST /modbus/send?pdu=... request line. */
static inline size_t gw_http_modbus_pdu(const char *p,size_t n,uint8_t *out,size_t max_out) {
    const char *line_end=(const char *)memchr(p,'\r',n);if(!line_end)return 0;
    const char *sp1=(const char *)memchr(p,' ',(size_t)(line_end-p));if(!sp1)return 0;
    const char *path=sp1+1;
    const char *sp2=(const char *)memchr(path,' ',(size_t)(line_end-path));if(!sp2)return 0;
    if((size_t)(sp2-path)<=17 || memcmp(path,"/modbus/send?pdu=",17))return 0;
    const char *hex=path+17;size_t hex_len=(size_t)(sp2-hex);
    if(!hex_len || (hex_len&1) || hex_len/2>max_out)return 0;
    for(size_t i=0;i<hex_len/2;i++) {
        int hi=gw_hex_nibble(hex[2*i]),lo=gw_hex_nibble(hex[2*i+1]);
        if(hi<0 || lo<0)return 0;
        out[i]=(uint8_t)((hi<<4)|lo);
    }
    return hex_len/2;
}
/* A clock step backwards must not repeat a day's successful synchronization. */
static inline bool gw_daily_sync_due(bool valid,uint64_t utc_us,uint64_t last_day) {
    return !valid || utc_us/UINT64_C(86400000000)>last_day;
}
