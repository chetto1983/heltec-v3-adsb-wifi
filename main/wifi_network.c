/* The radio capture and decoder run independently of network configuration.
 * The HTTP task commits a complete setting. The existing field task applies
 * it after the browser has received its response. */
#include "wifi_network.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct { char ssid[33], key[65]; } Router;
enum { APPLY_AP=1, APPLY_STA=2 };
static esp_netif_t *hotspot, *client;
static atomic_uint updates, last_reason;
static atomic_bool retry_link, check_subnet, has_ip;
static bool router_enabled;
static unsigned waiting;
static int64_t change_at, retry_at;
static const char *tag="wifi-settings";
#define TRY(call) ESP_RETURN_ON_ERROR((call),tag,#call)

static esp_err_t read_router(Router *router) {
    memset(router,0,sizeof(*router));
    nvs_handle_t nvs;
    TRY(nvs_open("adsb",NVS_READONLY,&nvs));
    size_t size=sizeof(*router);
    esp_err_t err=nvs_get_blob(nvs,"router_v1",router,&size);
    nvs_close(nvs);
    if (err==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err==ESP_OK && (size!=sizeof(*router) || router->ssid[32] || router->key[64]))
        return ESP_ERR_INVALID_SIZE;
    return err;
}

static void link_event(void *arg, esp_event_base_t base, int32_t event, void *data) {
    if (base==IP_EVENT && event==IP_EVENT_STA_GOT_IP) {
        atomic_store(&last_reason,0);
        atomic_store(&has_ip,true);
        atomic_store(&check_subnet,true);
        atomic_store(&retry_link,false);
    } else if (base==WIFI_EVENT && event==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *lost=data;
        atomic_store(&has_ip,false);
        atomic_store(&last_reason,lost->reason);
        atomic_store(&retry_link,true);
    } else if (base==IP_EVENT && event==IP_EVENT_STA_LOST_IP) {
        atomic_store(&has_ip,false);
    }
}

esp_err_t wifi_network_start(esp_netif_t *ap) {
    hotspot=ap;
    client=esp_netif_create_default_wifi_sta();
    if (!client) return ESP_ERR_NO_MEM;
    TRY(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,link_event,NULL));
    TRY(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,link_event,NULL));
    TRY(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_LOST_IP,link_event,NULL));
    atomic_store(&updates,APPLY_STA);
    return ESP_OK;
}

static esp_err_t apply_router(void) {
    Router r;
    TRY(read_router(&r));
    wifi_mode_t mode;
    TRY(esp_wifi_get_mode(&mode));
    router_enabled=r.ssid[0]!=0;
    if (mode==WIFI_MODE_APSTA) esp_wifi_disconnect();
    atomic_store(&has_ip,false);
    atomic_store(&last_reason,0);
    atomic_store(&retry_link,false);
    if (!router_enabled) {
        if (mode==WIFI_MODE_APSTA) return esp_wifi_set_mode(WIFI_MODE_AP);
        return ESP_OK;
    }
    if (mode!=WIFI_MODE_APSTA) TRY(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t cfg={0};
    memcpy(cfg.sta.ssid,r.ssid,strlen(r.ssid));
    memcpy(cfg.sta.password,r.key,strlen(r.key));
    cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable=true;
    cfg.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
    TRY(esp_wifi_set_config(WIFI_IF_STA,&cfg));
    return esp_wifi_connect();
}

static esp_err_t apply_ap_key(void) {
    wifi_config_t cfg;
    TRY(esp_wifi_get_config(WIFI_IF_AP,&cfg));
    nvs_handle_t nvs;
    TRY(nvs_open("adsb",NVS_READONLY,&nvs));
    size_t size=sizeof(cfg.ap.password);
    memset(cfg.ap.password,0,size);
    esp_err_t err=nvs_get_str(nvs,"ap_pass",(char *)cfg.ap.password,&size);
    nvs_close(nvs);
    if (err!=ESP_OK) return err;
    TRY(esp_wifi_set_config(WIFI_IF_AP,&cfg));
    esp_wifi_deauth_sta(0);
    return ESP_OK;
}

static esp_err_t separate_subnets(void) {
    esp_netif_ip_info_t lan, local;
    TRY(esp_netif_get_ip_info(client,&lan));
    TRY(esp_netif_get_ip_info(hotspot,&local));
    uint32_t mask=lan.netmask.addr & local.netmask.addr;
    if (!lan.ip.addr || (local.ip.addr&mask)!=(lan.ip.addr&mask)) return ESP_OK;
    esp_netif_set_ip4_addr(&local.ip,192,168,44,1);
    if ((local.ip.addr&mask)==(lan.ip.addr&mask)) esp_netif_set_ip4_addr(&local.ip,172,26,44,1);
    local.gw=local.ip;
    TRY(esp_netif_dhcps_stop(hotspot));
    esp_err_t err=esp_netif_set_ip_info(hotspot,&local);
    esp_err_t restarted=esp_netif_dhcps_start(hotspot);
    if (err!=ESP_OK) return err;
    if (restarted!=ESP_OK) return restarted;
    esp_wifi_deauth_sta(0); /* Phones need a new DHCP lease on the new subnet. */
    return ESP_OK;
}

void wifi_network_poll(void) {
    if (!client) return;
    int64_t now=esp_timer_get_time()/1000;
    unsigned changed=atomic_exchange(&updates,0);
    if (changed) { waiting|=changed; change_at=now+1200; }
    if (waiting && now>=change_at) {
        if ((waiting&APPLY_AP) && apply_ap_key()==ESP_OK) waiting&=~APPLY_AP;
        if ((waiting&APPLY_STA) && apply_router()==ESP_OK) waiting&=~APPLY_STA;
        change_at=retry_at=now+20000;
    }
    if (atomic_exchange(&check_subnet,false) && separate_subnets()!=ESP_OK)
        ESP_LOGW(tag,"Could not update hotspot subnet");
    if (router_enabled && now>=retry_at && atomic_exchange(&retry_link,false)) {
        retry_at=now+20000;
        if (esp_wifi_connect()!=ESP_OK) atomic_store(&retry_link,true);
    }
}

/* Restrict configuration to a DHCP client actually associated with this AP.
 * Requests arriving through the router can read status and aircraft data. */
static uint32_t ipv4_address(const struct sockaddr_storage *addr) {
    if (addr->ss_family==AF_INET) return ((const struct sockaddr_in *)addr)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    if (addr->ss_family==AF_INET6) {
        const struct in6_addr *ip=&((const struct sockaddr_in6 *)addr)->sin6_addr;
        static const uint8_t mapped[12]={0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (!memcmp(ip->s6_addr,mapped,sizeof(mapped))) {
            uint32_t v4;
            memcpy(&v4,&ip->s6_addr[12],sizeof(v4));
            return v4;
        }
    }
#endif
    return 0;
}

static bool direct_client(httpd_req_t *req) {
    struct sockaddr_storage local={0}, peer={0};
    socklen_t len=sizeof(local);
    int socket=httpd_req_to_sockfd(req);
    esp_netif_ip_info_t ip;
    if (getsockname(socket,(struct sockaddr *)&local,&len) ||
        esp_netif_get_ip_info(hotspot,&ip)!=ESP_OK || ipv4_address(&local)!=ip.ip.addr) return false;
    len=sizeof(peer);
    if (getpeername(socket,(struct sockaddr *)&peer,&len)) return false;
    uint32_t peer_ip=ipv4_address(&peer);
    if (!peer_ip) return false;
    wifi_sta_list_t stations;
    wifi_sta_mac_ip_list_t leases;
    if (esp_wifi_ap_get_sta_list(&stations)!=ESP_OK ||
        esp_wifi_ap_get_sta_list_with_ip(&stations,&leases)!=ESP_OK) return false;
    for (int i=0;i<leases.num;i++)
        if (leases.sta[i].ip.addr==peer_ip) return true;
    return false;
}

static void headers(httpd_req_t *req, const char *type) {
    httpd_resp_set_type(req,type);
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
}

esp_err_t wifi_settings_page(httpd_req_t *req) {
    extern const uint8_t start[] asm("_binary_wifi_html_start");
    extern const uint8_t end[] asm("_binary_wifi_html_end");
    headers(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"X-Frame-Options","DENY");
    return httpd_resp_send(req,(const char *)start,end-start);
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body) {
    headers(req,"application/json");
    httpd_resp_set_status(req,status);
    return httpd_resp_sendstr(req,body);
}

static const char *connection_status(const Router *r, bool connected) {
    if (!*r->ssid) return "disabled";
    if (connected) return "connected";
    unsigned reason=atomic_load(&last_reason);
    if (reason==WIFI_REASON_NO_AP_FOUND) return "not_found";
    if (reason==WIFI_REASON_AUTH_FAIL || reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason==WIFI_REASON_HANDSHAKE_TIMEOUT) return "auth_failed";
    return reason ? "retrying" : "connecting";
}

static bool router_address(const Router *r, esp_netif_ip_info_t *lan) {
    wifi_ap_record_t association;
    return atomic_load(&has_ip) && esp_netif_get_ip_info(client,lan)==ESP_OK &&
        lan->ip.addr && esp_wifi_sta_get_ap_info(&association)==ESP_OK &&
        !strncmp((const char *)association.ssid,r->ssid,sizeof(r->ssid));
}

void wifi_network_status(void) {
    Router r;
    esp_netif_ip_info_t local,lan={0};
    if (!client || read_router(&r)!=ESP_OK || esp_netif_get_ip_info(hotspot,&local)!=ESP_OK) {
        puts("SXNET unavailable");
        return;
    }
    bool connected=router_address(&r,&lan);
    wifi_sta_list_t peers;
    int clients=esp_wifi_ap_get_sta_list(&peers)==ESP_OK ? peers.num : -1;
    printf("SXNET state=%s ap=" IPSTR " router=" IPSTR " clients=%d\n",
           connection_status(&r,connected),IP2STR(&local.ip),IP2STR(&lan.ip),clients);
}

esp_err_t wifi_settings_get(httpd_req_t *req) {
    Router r;
    wifi_config_t ap;
    esp_netif_ip_info_t local,lan={0};
    if (read_router(&r)!=ESP_OK || esp_wifi_get_config(WIFI_IF_AP,&ap)!=ESP_OK ||
        esp_netif_get_ip_info(hotspot,&local)!=ESP_OK)
        return reply(req,"503 Service Unavailable","{\"error\":\"unavailable\"}");
    bool connected=router_address(&r,&lan);
    char ap_ip[16], lan_ip[16], body[560];
    snprintf(ap_ip,sizeof(ap_ip),IPSTR,IP2STR(&local.ip));
    snprintf(lan_ip,sizeof(lan_ip),IPSTR,IP2STR(&lan.ip));
    cJSON *json=cJSON_CreateObject();
    bool ok=json && cJSON_AddStringToObject(json,"ssid",(char *)ap.ap.ssid) &&
        cJSON_AddStringToObject(json,"ap_ip",ap_ip) && cJSON_AddBoolToObject(json,"can_manage",direct_client(req)) &&
        cJSON_AddStringToObject(json,"router_ssid",r.ssid) &&
        cJSON_AddStringToObject(json,"state",connection_status(&r,connected)) &&
        (connected ? cJSON_AddStringToObject(json,"router_ip",lan_ip) : cJSON_AddNullToObject(json,"router_ip")) &&
        cJSON_PrintPreallocated(json,body,sizeof(body),false);
    cJSON_Delete(json);
    return ok ? reply(req,"200 OK",body) : reply(req,"503 Service Unavailable","{\"error\":\"unavailable\"}");
}

static const char *field(cJSON *json, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json,name));
}
static bool valid_key(const char *key, bool raw_allowed) {
    if (!key) return false;
    size_t n=strlen(key);
    if (n<8 || n>(raw_allowed?64:63)) return false;
    for (size_t i=0;i<n;i++) {
        unsigned char c=key[i];
        if (c<32 || c>126 || (n==64 && !isxdigit(c))) return false;
    }
    return true;
}

static bool authenticated(const char *input) {
    char secret[64]={0}, padded[64]={0};
    nvs_handle_t nvs;
    if (!input || strlen(input)>63 || nvs_open("adsb",NVS_READONLY,&nvs)!=ESP_OK) return false;
    size_t n=sizeof(secret);
    esp_err_t err=nvs_get_str(nvs,"ap_pass",secret,&n);
    nvs_close(nvs);
    if (err!=ESP_OK) return false;
    memcpy(padded,input,strlen(input));
    volatile unsigned diff=0;
    for (unsigned i=0;i<sizeof(secret);i++) diff|=(uint8_t)secret[i]^(uint8_t)padded[i];
    return diff==0;
}

static esp_err_t save_action(cJSON *json, unsigned *change) {
    const char *action=field(json,"action"), *key=field(json,"password"), *ssid=field(json,"ssid");
    Router r={0};
    if (action && !strcmp(action,"change_ap")) {
        if (!valid_key(key,false)) return ESP_ERR_INVALID_ARG;
        *change=APPLY_AP;
    } else if (action && !strcmp(action,"join")) {
        if (!ssid || !strlen(ssid) || strlen(ssid)>32 || !valid_key(key,true)) return ESP_ERR_INVALID_ARG;
        for (const unsigned char *p=(const unsigned char *)ssid;*p;p++) if (*p<32 || *p==127) return ESP_ERR_INVALID_ARG;
        strcpy(r.ssid,ssid);
        strcpy(r.key,key);
        *change=APPLY_STA;
    } else if (action && !strcmp(action,"forget")) {
        *change=APPLY_STA;
    } else return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    TRY(nvs_open("adsb",NVS_READWRITE,&nvs));
    esp_err_t err=*change==APPLY_AP ? nvs_set_str(nvs,"ap_pass",key) : nvs_set_blob(nvs,"router_v1",&r,sizeof(r));
    if (err==ESP_OK) err=nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t wifi_settings_post(httpd_req_t *req) {
    if (!direct_client(req)) return reply(req,"403 Forbidden","{\"error\":\"hotspot_only\"}");
    char type[48],body[768];
    if (httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK ||
        strcmp(type,"application/json")) return reply(req,"415 Unsupported Media Type","{\"error\":\"json\"}");
    if (!req->content_len || req->content_len>=sizeof(body)) return reply(req,"400 Bad Request","{\"error\":\"input\"}");
    size_t done=0;
    while (done<req->content_len) {
        int n=httpd_req_recv(req,body+done,req->content_len-done);
        if (n<=0) return ESP_FAIL;
        done+=n;
    }
    body[done]=0;
    cJSON *json=NULL;
    if (!memchr(body,0,done) && !strstr(body,"\\u0000")) json=cJSON_ParseWithOpts(body,NULL,true);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return reply(req,"400 Bad Request","{\"error\":\"input\"}");
    }
    if (!authenticated(field(json,"current"))) {
        cJSON_Delete(json);
        return reply(req,"403 Forbidden","{\"error\":\"current_password\"}");
    }
    unsigned change=0;
    esp_err_t saved=save_action(json,&change);
    cJSON_Delete(json);
    if (saved==ESP_ERR_INVALID_ARG) return reply(req,"400 Bad Request","{\"error\":\"input\"}");
    if (saved!=ESP_OK) return reply(req,"503 Service Unavailable","{\"error\":\"storage\"}");
    esp_err_t sent=reply(req,"202 Accepted","{\"saved\":true}");
    atomic_fetch_or(&updates,change); /* Also apply a committed change if the response was lost. */
    return sent;
}
