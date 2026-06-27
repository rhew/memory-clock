#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_port_init(void);
esp_err_t display_port_show_monochrome_full(const uint8_t *buffer, size_t buffer_size,
                                            int width, int height);
esp_err_t display_port_show_monochrome_partial(const uint8_t *buffer, size_t buffer_size,
                                               int width, int height,
                                               int x, int y, int region_width, int region_height);
