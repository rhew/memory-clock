#pragma once

#include <stddef.h>

#include "esp_err.h"

const char *provisioning_ssid(void);
esp_err_t provisioning_start(char *ip_out, size_t ip_out_size);
