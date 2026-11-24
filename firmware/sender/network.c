#include "network.h"
#include "web_ui.h"
#include "../gateway/frontend.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"
#include <stdio.h>
#include <inttypes.h>
#if __has_include("../../.build/wifi.h")
#include "../../.build/wifi.h"
#elif __has_include("../wifi.local.h")
#include "../wifi.local.h"
#else
#include "../wifi.example.h"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#define CONNECTIONS 8
#define BUFFER_SIZE 1024

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t generation;
    bool http,closing,waiting,waiting_modbus;
    unsigned outstanding;
    uint64_t last_ms;
    size_t used,out_used,static_left;
    const uint8_t *static_out;
    uint8_t input[BUFFER_SIZE],output[BUFFER_SIZE];
} connection;
static connection clients[CONNECTIONS];
static struct udp_pcb *ntp;
static ip_addr_t ntp_address;
static bool connected,ntp_waiting,dns_waiting,ntp_event;
static uint64_t connect_try_ms,ntp_try_ms,ntp_sent_us,ntp_nonce,ntp_utc_us,ntp_mono_us,last_day;
static unsigned ntp_backoff=10;

static void drop(connection *c) {
    if(c->pcb) {tcp_arg(c->pcb,NULL);tcp_recv(c->pcb,NULL);tcp_err(c->pcb,NULL);tcp_abort(c->pcb);}
    c->pcb=NULL;c->used=c->out_used=c->static_left=0;c->static_out=NULL;c->waiting=c->waiting_modbus=false;c->outstanding=0;
}
static void broken(void *arg,err_t err) {(void)err;connection *c=arg;if(c){c->pcb=NULL;c->used=c->out_used=c->static_left=0;c->static_out=NULL;c->waiting=c->waiting_modbus=false;c->outstanding=0;}}
static err_t received(void *arg,struct tcp_pcb *pcb,struct pbuf *p,err_t err) {
    (void)pcb;connection *c=arg;
    if(!p) {drop(c);return ERR_ABRT;}
    if(err!=ERR_OK || p->tot_len>sizeof(c->input)-c->used) {pbuf_free(p);drop(c);return ERR_ABRT;}
    pbuf_copy_partial(p,c->input+c->used,p->tot_len,0);c->used+=p->tot_len;
    tcp_recved(c->pcb,p->tot_len);pbuf_free(p);c->last_ms=time_us_64()/1000;return ERR_OK;
}
static err_t accepted(void *arg,struct tcp_pcb *pcb,err_t err) {
    if(err!=ERR_OK)return err;
    bool http=arg!=NULL;unsigned first=http?4:0,last=http?8:4;
    for(unsigned i=first;i<last;i++)if(!clients[i].pcb) {
        connection *c=&clients[i];uint32_t generation=(c->generation+1)&0x00ffffffu;
        memset(c,0,sizeof(*c));c->generation=generation?generation:1;c->pcb=pcb;c->http=http;c->last_ms=time_us_64()/1000;
        tcp_arg(pcb,c);tcp_recv(pcb,received);tcp_err(pcb,broken);tcp_nagle_disable(pcb);return ERR_OK;
    }
    tcp_abort(pcb);return ERR_ABRT;
}
static bool listen_port(uint16_t port,bool http) {
    struct tcp_pcb *p=tcp_new();if(!p)return false;
    if(tcp_bind(p,IP_ADDR_ANY,port)!=ERR_OK){tcp_close(p);return false;}
    struct tcp_pcb *listener=tcp_listen(p);if(!listener){tcp_close(p);return false;}
    tcp_arg(listener,http?(void *)1:NULL);tcp_accept(listener,accepted);return true;
}
static uint32_t token(connection *c) {return (c->generation<<8)|(uint32_t)(c-clients+1);}
static connection *lookup(uint32_t id) {
    unsigned i=(id&255u);if(!i || i>CONNECTIONS)return NULL;
    connection *c=&clients[i-1];return c->pcb && c->generation==(id>>8)?c:NULL;
}
static void output(connection *c,const void *p,size_t n) {
    if(n>sizeof(c->output)-c->out_used){drop(c);return;}
    memcpy(c->output+c->out_used,p,n);c->out_used+=n;
}
static void modbus_response(connection *c,uint16_t id,const uint8_t *p,size_t n,uint8_t error) {
    uint8_t frame[260];size_t size=error?3:n;
    if(size>254 || size<2){drop(c);return;}
    frame[0]=(uint8_t)(id>>8);frame[1]=(uint8_t)id;frame[2]=frame[3]=0;
    frame[4]=(uint8_t)(size>>8);frame[5]=(uint8_t)size;
    if(error){frame[6]=p[0];frame[7]=p[1]|0x80;frame[8]=error;}
    else memcpy(frame+6,p,n);
    output(c,frame,size+6);
}
static void http_response(connection *c,unsigned status,const char *json) {
    char header[200];int n=snprintf(header,sizeof(header),"HTTP/1.1 %u %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",status,status==200?"OK":status==504?"Gateway Timeout":status==503?"Service Unavailable":status==404?"Not Found":"Bad Request",(unsigned)strlen(json));
    output(c,header,(size_t)n);if(c->pcb)output(c,json,strlen(json));c->closing=true;c->waiting=c->waiting_modbus=false;
}
static void http_html(connection *c,const char *html,size_t len) {
    char header[200];int n=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",(unsigned)len);
    output(c,header,(size_t)n);if(c->pcb){c->static_out=(const uint8_t *)html;c->static_left=len;}c->closing=true;c->waiting=c->waiting_modbus=false;
}
void gw_deliver(uint32_t client,uint16_t id,const uint8_t *p,size_t n,uint8_t error) {
    cyw43_arch_lwip_begin();connection *c=lookup(client);
    if(c) {
        if(!c->http){if(c->outstanding)--c->outstanding;modbus_response(c,id,p,n,error);}
        else if(c->waiting_modbus) {
            char json[700];
            if(error) {
                snprintf(json,sizeof(json),"{\"ok\":false,\"error\":%u,\"unit\":%u,\"function\":%u}",error,n>0?p[0]:0,n>1?p[1]:0);
            } else {
                static const char hx[]="0123456789abcdef";
                int pos=snprintf(json,sizeof(json),"{\"ok\":true,\"unit\":%u,\"function\":%u,\"hex\":\"",n>0?p[0]:0,n>1?p[1]:0);
                for(size_t i=0;i<n && pos+4<(int)sizeof(json);i++){json[pos++]=hx[p[i]>>4];json[pos++]=hx[p[i]&15];}
                json[pos++]='"';json[pos++]='}';json[pos]=0;
            }
            http_response(c,200,json);
        }
    }
    cyw43_arch_lwip_end();
}
void gw_control_done(uint32_t client,bool confirmed) {
    cyw43_arch_lwip_begin();connection *c=lookup(client);
    if(c && c->http)http_response(c,confirmed?200:504,confirmed?"{\"confirmed\":true}":"{\"confirmed\":false,\"outcome\":\"unknown\"}");
    cyw43_arch_lwip_end();
}
static void consume(connection *c,size_t n) {memmove(c->input,c->input+n,c->used-n);c->used-=n;}
static void parse_modbus(connection *c,gw_sender *s,uint64_t now) {
    while(c->pcb && c->used>=7 && c->outstanding<2) {
        size_t frame_size=0;int parsed=gw_mbap(c->input,c->used,&frame_size);
        if(parsed<0){drop(c);return;}if(!parsed)return;
        size_t n=frame_size-6;
        uint16_t id=(uint16_t)((c->input[0]<<8)|c->input[1]);
        uint8_t error=gw_sender_submit(s,token(c),id,c->input+6,n,now);
        if(error)modbus_response(c,id,c->input+6,n,error);else ++c->outstanding;
        if(c->pcb)consume(c,n+6);
    }
}
static const char *u64_fmt(uint64_t v,char buf[21]) {
    char *p=buf+20;*p=0;
    do {*--p=(char)('0'+(v%10));v/=10;} while(v);
    return p;
}
static void parse_http(connection *c,gw_sender *s,uint64_t now) {
    if(c->waiting || c->waiting_modbus || c->closing)return;
    if(c->used==sizeof(c->input)){http_response(c,400,"{\"error\":\"headers too large\"}");return;}
    c->input[c->used]=0;
    enum gw_http_route route=gw_http((char *)c->input,c->used);
    if(route==HTTP_INCOMPLETE)return;
    if(route==HTTP_BAD){http_response(c,400,"{\"error\":\"invalid request\"}");return;}
    char json[700];
    if(route==HTTP_INDEX) {
        http_html(c,WEB_UI_HTML,sizeof(WEB_UI_HTML)-1);
    } else if(route==HTTP_MODBUS) {
        uint8_t pdu[GW_DATA_MAX];
        size_t n=gw_http_modbus_pdu((char *)c->input,c->used,pdu,sizeof(pdu));
        if(n<2){http_response(c,400,"{\"ok\":false,\"error\":3}");return;}
        uint8_t err=gw_sender_submit(s,token(c),0,pdu,n,now);
        if(err){snprintf(json,sizeof(json),"{\"ok\":false,\"error\":%u,\"unit\":%u,\"function\":%u}",err,pdu[0],pdu[1]);http_response(c,err==6?503:400,json);}
        else c->waiting_modbus=true;
    } else if(route==HTTP_PAUSE || route==HTTP_RESUME) {
        if(!gw_sender_control(s,route==HTTP_PAUSE,token(c),now))http_response(c,503,"{\"error\":\"control pending\"}");
        else c->waiting=true;
    } else if(route==HTTP_STATUS) {
        char b0[21],b1[21],b2[21],b3[21],b4[21],b5[21],b6[21],b7[21];
        snprintf(json,sizeof(json),
            "{\"float_grounded\":%s,\"receiver_commanded_on\":%s,\"receiver_status_age_ms\":%s,"
            "\"receiver_pause_remaining_ms\":%s,\"last_on_ack_ms\":%s,\"receiver_session\":%s,"
            "\"wifi\":%s,\"pause_remaining_ms\":%s,\"clock_synced\":%s,\"utc_us\":%s,"
            "\"receiver_clock_synced\":%s,\"receiver_clock_sync_age_ms\":%s,\"clock_sync_age_ms\":%s,\"queued\":%u}",
            s->grounded?"true":"false",s->receiver_on?"true":"false",
            u64_fmt(s->receiver_status_ms?now-s->receiver_status_ms:0,b0),
            u64_fmt(s->receiver_pause_until_ms>now?s->receiver_pause_until_ms-now:0,b1),
            u64_fmt(s->last_on_ack_ms,b2),u64_fmt(s->peer,b3),connected?"true":"false",
            u64_fmt(s->pause_until_ms>now?s->pause_until_ms-now:0,b4),s->clock.valid?"true":"false",
            u64_fmt(gw_utc(&s->clock,now*1000),b5),s->receiver_clock_synced?"true":"false",
            u64_fmt(s->receiver_clock_synced?now-s->receiver_clock_sync_ms:0,b6),
            u64_fmt(s->clock.valid?now-s->clock.synced_ms:0,b7),s->count);
        http_response(c,200,json);
    } else if(route==HTTP_RESULT) {
        char b0[21],b1[21];
        snprintf(json,sizeof(json),
            "{\"transaction\":%u,\"tcp_transaction\":%u,\"client\":%u,\"unit\":%u,\"function\":%u,"
            "\"error\":%u,\"acquired_utc_us\":%s,\"synchronized\":%s,\"receiver_sync_age_ms\":%s}",
            (unsigned)s->completed_id,s->last_tcp_id,(unsigned)s->last_result_client,s->last_unit,
            s->last_function,s->last_error,u64_fmt(s->last_acquired_us,b0),
            s->last_result_synced?"true":"false",u64_fmt(s->last_sync_age,b1));
        http_response(c,200,json);
    } else http_response(c,404,"{\"error\":\"not found\"}");
}
static uint64_t ntp_time(const uint8_t *p) {
    uint64_t sec=((uint64_t)p[0]<<24)|((uint64_t)p[1]<<16)|((uint64_t)p[2]<<8)|p[3];
    uint32_t frac=((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7];
    if(sec<2208988800ULL)sec+=UINT64_C(4294967296);
    return (sec-2208988800ULL)*1000000+((uint64_t)frac*1000000>>32);
}
static void ntp_reply(void *arg,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *addr,u16_t port) {
    (void)arg;(void)pcb;uint8_t data[48];uint64_t now=time_us_64();
    bool valid=ntp_waiting && port==123 && ip_addr_cmp(addr,&ntp_address) && p->tot_len>=48;
    if(valid)pbuf_copy_partial(p,data,48,0);
    pbuf_free(p);
    if(!valid || (data[0]&7)!=4 || (data[0]>>6)==3 || data[1]==0 || data[1]>15 || gw_get64(data+24)!=ntp_nonce)return;
    uint64_t transmit=ntp_time(data+40),receive=ntp_time(data+32),elapsed=now-ntp_sent_us;
    if(transmit<UINT64_C(1700000000000000) || transmit<receive || transmit-receive>elapsed || elapsed>10000000)return;
    ntp_utc_us=transmit+(elapsed-(transmit-receive))/2;ntp_mono_us=now;ntp_event=true;ntp_waiting=false;ntp_backoff=10;
}
static void ntp_send(const ip_addr_t *address) {
    if(!address){dns_waiting=false;return;}
    ntp_address=*address;uint8_t data[48]={0x23};
    ntp_nonce=++ntp_sent_us+time_us_64();gw_put64(data+40,ntp_nonce);
    struct pbuf *p=pbuf_alloc(PBUF_TRANSPORT,sizeof(data),PBUF_RAM);
    if(p){pbuf_take(p,data,sizeof(data));ntp_sent_us=time_us_64();
        ntp_waiting=udp_sendto(ntp,p,address,123)==ERR_OK;pbuf_free(p);}
    dns_waiting=false;
}
static void resolved(const char *name,const ip_addr_t *addr,void *arg) {(void)name;(void)arg;ntp_send(addr);}
bool network_init(void) {
    if(!WIFI_SSID[0])printf("Wi-Fi unconfigured: edit firmware/wifi.local.h; float/radio operation continues.\n");
    if(cyw43_arch_init()!=0)return false;
    cyw43_arch_enable_sta_mode();cyw43_arch_lwip_begin();
    bool ok=listen_port(502,false) && listen_port(80,true);
    ntp=udp_new();if(ntp)udp_recv(ntp,ntp_reply,NULL);else ok=false;
    if(netif_default) {
        netif_set_hostname(netif_default,DEVICE_HOSTNAME);
        mdns_resp_init();
        if(mdns_resp_add_netif(netif_default,DEVICE_HOSTNAME)==ERR_OK) {
            mdns_resp_add_service(netif_default,DEVICE_HOSTNAME,"_http",DNSSD_PROTO_TCP,80,NULL,NULL);
            mdns_resp_add_service(netif_default,DEVICE_HOSTNAME,"_modbus",DNSSD_PROTO_TCP,502,NULL,NULL);
        }
    }
    cyw43_arch_lwip_end();return ok;
}
bool network_connected(void){return connected;}
void network_poll(gw_sender *s,uint64_t now_us) {
    uint64_t now=now_us/1000;
    if(WIFI_SSID[0] && now>=connect_try_ms) {
        bool was_connected=connected;
        connected=cyw43_tcpip_link_status(&cyw43_state,CYW43_ITF_STA)==CYW43_LINK_UP;
        if(!connected){cyw43_arch_wifi_connect_async(WIFI_SSID,WIFI_PASSWORD,WIFI_PASSWORD[0]?CYW43_AUTH_WPA2_AES_PSK:CYW43_AUTH_OPEN);connect_try_ms=now+30000;}
        else {
            connect_try_ms=now+1000;
            if(!was_connected) {
                cyw43_arch_lwip_begin();
                if(netif_default) {
                    printf("Wi-Fi IP %s (" DEVICE_HOSTNAME ".local)\n",ip4addr_ntoa(netif_ip4_addr(netif_default)));
                    mdns_resp_restart(netif_default);
                }
                cyw43_arch_lwip_end();
            }
        }

    }
    cyw43_arch_lwip_begin();
    if(ntp_event){ntp_event=false;gw_sender_ntp(s,ntp_mono_us,ntp_utc_us);last_day=ntp_utc_us/UINT64_C(86400000000);}
    if(connected && ntp && gw_daily_sync_due(s->clock.valid,gw_utc(&s->clock,now_us),last_day) && now>=ntp_try_ms && !ntp_waiting && !dns_waiting) {
        ntp_try_ms=now+(uint64_t)ntp_backoff*1000;if(ntp_backoff<600)ntp_backoff*=2;if(ntp_backoff>600)ntp_backoff=600;
        dns_waiting=true;ip_addr_t addr;err_t err=dns_gethostbyname(NTP_SERVER,&addr,resolved,NULL);
        if(err==ERR_OK)ntp_send(&addr);else if(err!=ERR_INPROGRESS)dns_waiting=false;
    }
    if((ntp_waiting || dns_waiting) && now>=ntp_try_ms){ntp_waiting=false;dns_waiting=false;}
    for(unsigned i=0;i<CONNECTIONS;i++) {
        connection *c=&clients[i];if(!c->pcb)continue;
        if(now-c->last_ms>CLIENT_QUEUE_MS+210000){drop(c);continue;}
        if(c->http && c->waiting && now-c->last_ms>=120000)
            http_response(c,504,"{\"confirmed\":false,\"outcome\":\"unknown\"}");
        if(c->http)parse_http(c,s,now);else parse_modbus(c,s,now);
        if(!c->pcb)continue;
        if(c->out_used) {
            size_t n=c->out_used;if(n>tcp_sndbuf(c->pcb))n=tcp_sndbuf(c->pcb);
            if(n && tcp_write(c->pcb,c->output,(u16_t)n,TCP_WRITE_FLAG_COPY)==ERR_OK) {
                memmove(c->output,c->output+n,c->out_used-n);c->out_used-=n;tcp_output(c->pcb);
            }
        } else if(c->static_left) {
            size_t n=c->static_left;if(n>tcp_sndbuf(c->pcb))n=tcp_sndbuf(c->pcb);
            if(n && tcp_write(c->pcb,c->static_out,(u16_t)n,0)==ERR_OK) {
                c->static_out+=n;c->static_left-=n;tcp_output(c->pcb);
            }
        }
        if(c->closing && !c->out_used && !c->static_left) {
            tcp_arg(c->pcb,NULL);tcp_recv(c->pcb,NULL);tcp_err(c->pcb,NULL);
            if(tcp_close(c->pcb)==ERR_OK)c->pcb=NULL;
            else {tcp_arg(c->pcb,c);tcp_recv(c->pcb,received);tcp_err(c->pcb,broken);}
        }
    }
    cyw43_arch_lwip_end();
}
