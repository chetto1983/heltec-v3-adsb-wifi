#pragma once
#include "esp_http_server.h"
#include "esp_netif.h"

esp_err_t wifi_network_start(esp_netif_t *ap);
void wifi_network_poll(void);
void wifi_network_status(void);
esp_err_t wifi_settings_page(httpd_req_t *req);
esp_err_t wifi_settings_get(httpd_req_t *req);
esp_err_t wifi_settings_post(httpd_req_t *req);
