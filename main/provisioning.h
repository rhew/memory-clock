#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

const char *provisioning_ssid(void);
bool provisioning_is_connected(void);
bool provisioning_get_rssi(int8_t *rssi_out);
esp_err_t provisioning_start(char *ip_out, size_t ip_out_size);
