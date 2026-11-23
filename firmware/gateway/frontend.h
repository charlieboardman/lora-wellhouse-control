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
enum gw_http_route { HTTP_INCOMPLETE,HTTP_BAD,HTTP_UNKNOWN,HTTP_PAUSE,HTTP_RESUME,HTTP_STATUS,HTTP_RESULT };
/* Caller supplies a terminated buffer; byte count excludes its terminator. */
static inline enum gw_http_route gw_http(const char *p,size_t n) {
    const char *end=strstr(p,"\r\n\r\n");if(!end)return HTTP_INCOMPLETE;
    const char *line_end=strstr(p,"\r\n");if(!line_end || line_end==p)return HTTP_BAD;
    char method[8],path[80],version[16],extra;
    /* Bound parsing to the request line, not following headers. */
    size_t first=(size_t)(line_end-p);if(first>=128)return HTTP_BAD;
    char line[128];memcpy(line,p,first);line[first]=0;
    if(sscanf(line,"%7s %79s %15s %c",method,path,version,&extra)!=3 ||
       (strcmp(version,"HTTP/1.1") && strcmp(version,"HTTP/1.0")))return HTTP_BAD;
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
    if(!strcmp(method,"POST") && !strcmp(path,"/pump/pause"))return HTTP_PAUSE;
    if(!strcmp(method,"POST") && !strcmp(path,"/pump/resume"))return HTTP_RESUME;
    if(!strcmp(method,"GET") && !strcmp(path,"/pump/status"))return HTTP_STATUS;
    if(!strcmp(method,"GET") && !strcmp(path,"/modbus/last-result"))return HTTP_RESULT;
    return HTTP_UNKNOWN;
}
/* A clock step backwards must not repeat a day's successful synchronization. */
static inline bool gw_daily_sync_due(bool valid,uint64_t utc_us,uint64_t last_day) {
    return !valid || utc_us/UINT64_C(86400000000)>last_day;
}
