#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BANNER_WIDTH 800
#define BANNER_HEIGHT 480
#define BANNER_BUFFER_SIZE ((BANNER_WIDTH * BANNER_HEIGHT) / 8)

typedef struct {
    int minute_x;
    int minute_y;
    int minute_width;
    int minute_height;
} banner_clock_layout_t;

void banner_render_status(uint8_t *buffer, size_t buffer_size, const char *headline,
                          const char *detail);
size_t banner_page_count(void);
void banner_render_page(uint8_t *buffer, size_t buffer_size, size_t page_index,
                        const char *weekday, const char *daypart, int hour12,
                        int minute, bool is_pm, const char *date_text,
                        uint32_t status_flags,
                        banner_clock_layout_t *layout);
