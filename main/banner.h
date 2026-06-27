#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BANNER_WIDTH 800
#define BANNER_HEIGHT 480
#define BANNER_BUFFER_SIZE ((BANNER_WIDTH * BANNER_HEIGHT) / 8)

void banner_render_status(uint8_t *buffer, size_t buffer_size, const char *headline,
                          const char *detail);
void banner_render_clock(uint8_t *buffer, size_t buffer_size, const char *weekday,
                         const char *daypart, int hour12, int minute, bool is_pm,
                         const char *date_text);
