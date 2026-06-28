#include "banner.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "font_assets.h"
#include "font_render.h"
#include "logo_bits.xbm"

enum {
    STATUS_LOGO_X = 352,
    STATUS_LOGO_Y = 64,
    STATUS_TITLE_Y = 184,
    STATUS_HEADLINE_Y = 276,
    STATUS_DETAIL_Y = 332,
    CLOCK_WEEKDAY_Y = 26,
    CLOCK_DAYPART_Y = 96,
    CLOCK_TIME_Y = 158,
    CLOCK_DATE_Y = 336,
    CLOCK_AMPM_GAP = 18,
};

static void set_pixel(uint8_t *buffer, int x, int y, bool black)
{
    if(x < 0 || x >= BANNER_WIDTH || y < 0 || y >= BANNER_HEIGHT) return;
    size_t index = (size_t)y * BANNER_WIDTH + (size_t)x;
    uint8_t mask = (uint8_t)(0x80 >> (index & 7));
    if(black) {
        buffer[index / 8] &= (uint8_t)~mask;
    } else {
        buffer[index / 8] |= mask;
    }
}

static void clear_buffer(uint8_t *buffer, size_t buffer_size)
{
    memset(buffer, 0xff, buffer_size);
}

static void draw_logo(uint8_t *buffer, int x, int y)
{
    int stride = (logo_bits_width + 7) / 8;
    for(int row = 0; row < logo_bits_height; ++row) {
        for(int col = 0; col < logo_bits_width; ++col) {
            unsigned char byte = (unsigned char)logo_bits_bits[row * stride + col / 8];
            if((byte & (1 << (col & 7))) != 0) {
                set_pixel(buffer, x + col, y + row, true);
            }
        }
    }
}

static void draw_text_centered(uint8_t *buffer, const memory_clock_font_t *font,
                               int y, const char *text)
{
    memory_clock_rect_t bounds = {0};
    font_measure_text_bounds(font, 0, y, text, &bounds);
    int x = (BANNER_WIDTH - bounds.width) / 2 - bounds.x;
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, font, x, y, text, true);
}

void banner_render_status(uint8_t *buffer, size_t buffer_size, const char *headline,
                          const char *detail)
{
    if(buffer_size < BANNER_BUFFER_SIZE) return;

    clear_buffer(buffer, buffer_size);
    draw_logo(buffer, STATUS_LOGO_X, STATUS_LOGO_Y);
    draw_text_centered(buffer, &memory_clock_font_medium, STATUS_TITLE_Y, "Memory Clock");
    draw_text_centered(buffer, &memory_clock_font_small, STATUS_HEADLINE_Y, headline);
    draw_text_centered(buffer, &memory_clock_font_small, STATUS_DETAIL_Y, detail);
}

void banner_render_clock(uint8_t *buffer, size_t buffer_size, const char *weekday,
                         const char *daypart, int hour12, int minute, bool is_pm,
                         const char *date_text, banner_clock_layout_t *layout)
{
    if(buffer_size < BANNER_BUFFER_SIZE) return;

    clear_buffer(buffer, buffer_size);

    draw_text_centered(buffer, &memory_clock_font_medium, CLOCK_WEEKDAY_Y, weekday);
    draw_text_centered(buffer, &memory_clock_font_small, CLOCK_DAYPART_Y, daypart);
    draw_text_centered(buffer, &memory_clock_font_small, CLOCK_DATE_Y, date_text);

    char hour_text[3];
    char minute_text[3];
    snprintf(hour_text, sizeof(hour_text), "%d", hour12);
    snprintf(minute_text, sizeof(minute_text), "%02d", minute);

    int hour_field_width = font_measure_text_width(&memory_clock_font_large, "88");
    int colon_width = font_measure_text_width(&memory_clock_font_large, ":");
    int minute_width = font_measure_text_width(&memory_clock_font_large, "88");
    int ampm_width = font_measure_text_width(&memory_clock_font_small, "pm");
    int total_width = hour_field_width + colon_width + minute_width + CLOCK_AMPM_GAP + ampm_width;
    int row_x = (BANNER_WIDTH - total_width) / 2;

    int hour_width = font_measure_text_width(&memory_clock_font_large, hour_text);
    int hour_x = row_x + hour_field_width - hour_width;
    int colon_x = row_x + hour_field_width;
    int minute_x = colon_x + colon_width;
    int ampm_x = minute_x + minute_width + CLOCK_AMPM_GAP;

    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_large,
                     hour_x, CLOCK_TIME_Y, hour_text, true);
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_large,
                     colon_x, CLOCK_TIME_Y, ":", true);
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_large,
                     minute_x, CLOCK_TIME_Y, minute_text, true);
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_small,
                     ampm_x, CLOCK_TIME_Y + 68, is_pm ? "pm" : "am", true);

    if(layout != NULL) {
        memory_clock_rect_t bounds = {0};
        font_measure_text_bounds(&memory_clock_font_large, minute_x, CLOCK_TIME_Y,
                                 "88", &bounds);
        layout->minute_x = bounds.x;
        layout->minute_y = bounds.y;
        layout->minute_width = bounds.width;
        layout->minute_height = bounds.height;
    }
}
