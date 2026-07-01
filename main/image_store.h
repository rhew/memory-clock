#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t *bits;
    int width;
    int height;
} memory_clock_image_t;

typedef struct {
    memory_clock_image_t *images;
    size_t count;
} memory_clock_image_set_t;

typedef enum {
    IMAGE_STORE_NO_APPOINTMENTS,
    IMAGE_STORE_HAS_APPOINTMENTS,
    IMAGE_STORE_SERVER_UNAVAILABLE,
} image_store_status_t;

esp_err_t image_store_init(void);
void image_store_lock(void);
void image_store_unlock(void);
size_t image_store_count(void);
const memory_clock_image_t *image_store_get(size_t index);
image_store_status_t image_store_status(void);
void image_store_replace(memory_clock_image_set_t *replacement);
void image_store_mark_server_unavailable_if_empty(void);
void image_store_free_set(memory_clock_image_set_t *set);
