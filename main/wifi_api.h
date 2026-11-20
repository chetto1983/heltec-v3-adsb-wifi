#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t boot, dial_hz, bursts, radio_restarts;
    bool ready;
} ReceiverStatus;

ReceiverStatus receiver_status(void);
esp_err_t wifi_api_start(void);
void wifi_api_sync_history(void);
void wifi_api_info(void);
