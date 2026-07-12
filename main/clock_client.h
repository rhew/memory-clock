#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    CLOCK_CLIENT_ERROR = -1,
    CLOCK_CLIENT_NOT_MODIFIED = 0,
    CLOCK_CLIENT_UPDATED = 1,
} clock_client_result_t;

typedef struct {
    int battery_mv;
    int8_t wifi_rssi;
    int32_t last_interaction_seconds;
    uint32_t uptime_seconds;
} clock_client_telemetry_t;

clock_client_result_t clock_client_poll(const clock_client_telemetry_t *telemetry);
