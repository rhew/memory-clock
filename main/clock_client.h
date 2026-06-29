#pragma once

#include "esp_err.h"

typedef enum {
    CLOCK_CLIENT_ERROR = -1,
    CLOCK_CLIENT_NOT_MODIFIED = 0,
    CLOCK_CLIENT_UPDATED = 1,
} clock_client_result_t;

clock_client_result_t clock_client_poll(void);
