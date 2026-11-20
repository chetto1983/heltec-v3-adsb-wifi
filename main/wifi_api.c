#include "wifi_api.h"
#include "history.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static char ssid[24], passphrase[13];
static esp_netif_t *ap;
static httpd_handle_t server;
static portMUX_TYPE cache_lock=portMUX_INITIALIZER_UNLOCKED;
static Card published[CARDS], response_cards[CARDS];
static unsigned published_count, published_generation;

void wifi_api_sync_history(void) {
    unsigned generation=history_generation();
    if (generation==published_generation) return;
    portENTER_CRITICAL(&cache_lock);
    published_count=history_copy_cards(published);
    portEXIT_CRITICAL(&cache_lock);
    published_generation=generation;
}

/* All URI handlers execute on one HTTP task. No socket operation takes
 * place while holding the receiver's snapshot lock. */
static unsigned snapshot(void) {
    portENTER_CRITICAL(&cache_lock);
    unsigned count=published_count;
    memcpy(response_cards,published,count*sizeof(Card));
    portEXIT_CRITICAL(&cache_lock);
    return count;
}

static void json_headers(httpd_req_t *req) {
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
}

static esp_err_t index_get(httpd_req_t *req) {
    json_headers(req);
    return httpd_resp_sendstr(req,"{\"status\":\"/api/status\",\"aircraft\":\"/api/aircraft\",\"messages\":\"/api/messages\"}");
}

static esp_err_t status_get(httpd_req_t *req) {
    ReceiverStatus s=receiver_status();
    char reply[256];
    snprintf(reply,sizeof(reply),
        "{\"boot_id\":\"%08" PRIx32 "\",\"uptime_ms\":%" PRId64 ",\"radio_ready\":%s,"
        "\"signal_hz\":1090000000,\"dial_hz\":%" PRIu32 ",\"bursts\":%" PRIu32 ","
        "\"radio_restarts\":%" PRIu32 ",\"history_capacity\":%u}",
        s.boot,(int64_t)(esp_timer_get_time()/1000),s.ready?"true":"false",
        s.dial_hz,s.bursts,s.radio_restarts,(unsigned)CARDS);
    json_headers(req);
    return httpd_resp_sendstr(req,reply);
}

static bool appears_later(unsigned index, unsigned count) {
    for (unsigned j=index+1; j<count; j++) {
        const Card *newer=&response_cards[j];
        if (newer->word[0]>>3!=2 && !memcmp(newer->word+1,response_cards[index].word+1,3))
            return true;
    }
    return false;
}

/* The decoded strings come exclusively from the decoder's callsign
 * alphabet (A-Z, digits and spaces) and its fixed presentation formats. */
static void string_or_null(char *dst, size_t size, const char *s) {
    if (*s) snprintf(dst,size,"\"%s\"",s);
    else snprintf(dst,size,"null");
}

static esp_err_t list_get(httpd_req_t *req) {
    bool aircraft=req->user_ctx!=NULL, first=true;
    unsigned count=snapshot();
    uint32_t boot=receiver_status().boot;
    int64_t now=esp_timer_get_time()/1000;
    json_headers(req);
    esp_err_t err=httpd_resp_sendstr_chunk(req,aircraft?"{\"aircraft\":[":"{\"messages\":[");
    for (unsigned i=count; err==ESP_OK && i>0; ) {
        const Card *c=&response_cards[--i];
        if (c->word[0]>>3==2 || (aircraft && appears_later(i,count))) continue;
        char age[24]="null", body[448], extra[300];
        if (c->boot==boot) snprintf(age,sizeof(age),"%" PRId64,(now>c->ms?now-c->ms:0)/1000);
        if (aircraft) {
            AircraftFields f;
            history_aircraft_fields(response_cards,i,&f);
            char name[16], height[36], speed[36], position[110]="null";
            string_or_null(name,sizeof(name),f.callsign);
            string_or_null(height,sizeof(height),f.altitude);
            string_or_null(speed,sizeof(speed),f.speed);
            if (f.has_position)
                snprintf(position,sizeof(position),"{\"lat\":%.5f,\"lon\":%.5f,\"fix_ms\":%" PRId64 "}",
                         f.lat,f.lon,f.position_ms);
            snprintf(extra,sizeof(extra),"\"callsign\":%s,\"altitude\":%s,\"speed\":%s,\"position\":%s",
                     name,height,speed,position);
        } else {
            char word[29];
            for (unsigned b=0; b<c->length; b++) snprintf(word+2*b,3,"%02X",c->word[b]);
            snprintf(extra,sizeof(extra),"\"df\":%u,\"hex\":\"%s\"",(unsigned)c->word[0]>>3,word);
        }
        int n=snprintf(body,sizeof(body),
            "%s{\"icao\":\"%02X%02X%02X\",\"boot_id\":\"%08" PRIx32 "\","
            "\"seen_ms\":%" PRId64 ",\"age_s\":%s,%s}",first?"":",",
            c->word[1],c->word[2],c->word[3],c->boot,c->ms,age,extra);
        if (n<0 || (size_t)n>=sizeof(body)) return ESP_FAIL;
        err=httpd_resp_send_chunk(req,body,n);
        first=false;
    }
    if (err==ESP_OK) err=httpd_resp_sendstr_chunk(req,"]}");
    if (err==ESP_OK) err=httpd_resp_send_chunk(req,NULL,0);
    return err;
}

static esp_err_t load_passphrase(void) {
    nvs_handle_t handle;
    esp_err_t err=nvs_open("adsb",NVS_READWRITE,&handle);
    if (err!=ESP_OK) return err;
    size_t size=sizeof(passphrase);
    err=nvs_get_str(handle,"ap_pass",passphrase,&size);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        static const char alphabet[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint8_t bytes[sizeof(passphrase)-1];
        esp_fill_random(bytes,sizeof(bytes));
        for (size_t i=0;i<sizeof(bytes);i++) passphrase[i]=alphabet[bytes[i]&31];
        passphrase[sizeof(bytes)]=0;
        err=nvs_set_str(handle,"ap_pass",passphrase);
        if (err==ESP_OK) err=nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

void wifi_api_info(void) {
    if (!server) { puts("SXWIFI unavailable"); return; }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap,&ip)==ESP_OK)
        printf("SXWIFI ssid=%s password=%s ip=" IPSTR "\n",ssid,passphrase,IP2STR(&ip.ip));
}

#define TRY(call) ESP_RETURN_ON_ERROR((call),"wifi-api",#call)
esp_err_t wifi_api_start(void) {
    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    ap=esp_netif_create_default_wifi_ap();
    if (!ap) return ESP_ERR_NO_MEM;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&init));
    TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    TRY(esp_wifi_set_mode(WIFI_MODE_AP));
    uint8_t mac[6];
    TRY(esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP));
    snprintf(ssid,sizeof(ssid),"Heltec-ADSB-%02X%02X",mac[4],mac[5]);
    TRY(load_passphrase());
    wifi_config_t config={.ap={.channel=1,.authmode=WIFI_AUTH_WPA2_PSK,.max_connection=3}};
    memcpy(config.ap.ssid,ssid,strlen(ssid)+1);
    memcpy(config.ap.password,passphrase,strlen(passphrase)+1);
    TRY(esp_wifi_set_config(WIFI_IF_AP,&config));
    TRY(esp_wifi_start());
    httpd_config_t http=HTTPD_DEFAULT_CONFIG();
    http.max_open_sockets=3;
    http.lru_purge_enable=true;
    http.stack_size=6144;
    http.send_wait_timeout=3;
    http.recv_wait_timeout=3;
    TRY(httpd_start(&server,&http));
    static const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=index_get},
        {.uri="/api/status",.method=HTTP_GET,.handler=status_get},
        {.uri="/api/aircraft",.method=HTTP_GET,.handler=list_get,.user_ctx=(void *)1},
        {.uri="/api/messages",.method=HTTP_GET,.handler=list_get}
    };
    for (unsigned i=0; i<sizeof(routes)/sizeof(routes[0]); i++)
        TRY(httpd_register_uri_handler(server,&routes[i]));
    wifi_api_info();
    return ESP_OK;
}
