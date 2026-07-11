#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    STATUS_FLAG_WIFI_OFF = 1 << 0,
    STATUS_FLAG_CLOUD_OFF = 1 << 1,
    STATUS_FLAG_BATTERY_LOW = 1 << 2,
} status_flag_t;

esp_err_t status_init(void);
bool status_set_wifi_connected(bool connected);
bool status_set_server_reachable(bool reachable);
bool status_sample_battery(void);
uint32_t status_flags(void);
bool status_take_changed(void);
