#include "banner.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "font_assets.h"
#include "font_render.h"
#include "image_store.h"
#include "logo_bits.xbm"

enum {
    WIDGET_WIDTH = BANNER_WIDTH / 2,
    WIDGET_HEIGHT = BANNER_HEIGHT,
    RIGHT_WIDGET_X = WIDGET_WIDTH,
    SEPARATOR_WIDTH = 4,
    SEPARATOR_RADIUS = SEPARATOR_WIDTH / 2,
    SEPARATOR_X = (BANNER_WIDTH - SEPARATOR_WIDTH) / 2,
    SEPARATOR_Y = 32,
    SEPARATOR_HEIGHT = BANNER_HEIGHT - (SEPARATOR_Y * 2),
    CLOCK_TEXT_LEFT_GUTTER = 4,
    CLOCK_TEXT_RIGHT_GUTTER = 8,
    CLOCK_TEXT_X = CLOCK_TEXT_LEFT_GUTTER,
    CLOCK_TEXT_WIDTH = WIDGET_WIDTH - CLOCK_TEXT_LEFT_GUTTER - CLOCK_TEXT_RIGHT_GUTTER,
    CLOCK_TIME_LEFT_GUTTER = 4,
    CLOCK_TIME_RIGHT_GUTTER = 28,
    CLOCK_TIME_X = CLOCK_TIME_LEFT_GUTTER,
    CLOCK_TIME_WIDTH = WIDGET_WIDTH - CLOCK_TIME_LEFT_GUTTER - CLOCK_TIME_RIGHT_GUTTER,
    CLOCK_TIME_SHIFT_X = -2,
    STATUS_LOGO_X = 352,
    STATUS_LOGO_Y = 64,
    STATUS_TITLE_Y = 184,
    STATUS_HEADLINE_Y = 276,
    STATUS_DETAIL_Y = 332,
    CLOCK_WEEKDAY_Y = 26,
    CLOCK_DAYPART_Y = 96,
    CLOCK_TIME_Y = 158,
    CLOCK_DATE_Y = 336,
    CLOCK_WIDGET_AMPM_GAP = 0,
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

static bool image_pixel_is_black(const uint8_t *buffer, int image_width,
                                 int image_height, int x, int y)
{
    if(x < 0 || x >= image_width || y < 0 || y >= image_height) return false;
    size_t stride = (size_t)(image_width + 7) / 8;
    size_t byte_index = (size_t)y * stride + (size_t)x / 8;
    uint8_t mask = (uint8_t)(1 << (x & 7));
    return (buffer[byte_index] & mask) != 0;
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

static void draw_text_centered_in_rect(uint8_t *buffer, const memory_clock_font_t *font,
                                       int rect_x, int rect_width, int y,
                                       const char *text)
{
    memory_clock_rect_t bounds = {0};
    font_measure_text_bounds(font, 0, y, text, &bounds);
    int x = rect_x + (rect_width - bounds.width) / 2 - bounds.x;
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, font, x, y, text, true);
}

static void draw_rounded_separator(uint8_t *buffer)
{
    int radius_squared = SEPARATOR_RADIUS * SEPARATOR_RADIUS;
    int center_x = SEPARATOR_X + SEPARATOR_RADIUS - 1;
    int top_center_y = SEPARATOR_Y + SEPARATOR_RADIUS - 1;
    int bottom_center_y = SEPARATOR_Y + SEPARATOR_HEIGHT - SEPARATOR_RADIUS;

    for(int y = SEPARATOR_Y; y < SEPARATOR_Y + SEPARATOR_HEIGHT; ++y) {
        for(int x = SEPARATOR_X; x < SEPARATOR_X + SEPARATOR_WIDTH; ++x) {
            bool black = true;
            if(y < SEPARATOR_Y + SEPARATOR_RADIUS) {
                int dx = x - center_x;
                int dy = y - top_center_y;
                black = (dx * dx + dy * dy) <= radius_squared;
            } else if(y >= SEPARATOR_Y + SEPARATOR_HEIGHT - SEPARATOR_RADIUS) {
                int dx = x - center_x;
                int dy = y - bottom_center_y;
                black = (dx * dx + dy * dy) <= radius_squared;
            }
            if(black) set_pixel(buffer, x, y, true);
        }
    }
}

static void draw_image_fit(uint8_t *buffer, int widget_x, int widget_y,
                           int widget_width, int widget_height,
                           const uint8_t *image, int image_width, int image_height)
{
    if(image == NULL || image_width <= 0 || image_height <= 0
       || widget_width <= 0 || widget_height <= 0) {
        return;
    }

    int draw_width;
    int draw_height;
    if((int64_t)image_width * widget_height > (int64_t)image_height * widget_width) {
        draw_width = widget_width;
        draw_height = (image_height * widget_width) / image_width;
    } else {
        draw_height = widget_height;
        draw_width = (image_width * widget_height) / image_height;
    }
    if(draw_width < 1) draw_width = 1;
    if(draw_height < 1) draw_height = 1;

    int dest_x = widget_x + (widget_width - draw_width) / 2;
    int dest_y = widget_y + (widget_height - draw_height) / 2;
    for(int y = 0; y < draw_height; ++y) {
        int source_y = (y * image_height) / draw_height;
        for(int x = 0; x < draw_width; ++x) {
            int source_x = (x * image_width) / draw_width;
            if(image_pixel_is_black(image, image_width, image_height, source_x, source_y)) {
                set_pixel(buffer, dest_x + x, dest_y + y, true);
            }
        }
    }
}

static void draw_image_widget(uint8_t *buffer, int widget_x, size_t image_index)
{
    const memory_clock_image_t *image = image_store_get(image_index);
    if(image == NULL) return;

    draw_image_fit(buffer, widget_x, 0, WIDGET_WIDTH, WIDGET_HEIGHT,
                   image->bits, image->width, image->height);
}

static void draw_no_appointments_widget(uint8_t *buffer, int widget_x)
{
    draw_text_centered_in_rect(buffer, &memory_clock_font_ui_small, widget_x + 28,
                               WIDGET_WIDTH - 56, 226, "No Appointments");
}

static void draw_server_unavailable_widget(uint8_t *buffer, int widget_x)
{
    draw_logo(buffer, widget_x + (WIDGET_WIDTH - logo_bits_width) / 2, 96);
    draw_text_centered_in_rect(buffer, &memory_clock_font_ui_small, widget_x + 28,
                               WIDGET_WIDTH - 56, 250, "couldn't connect");
    draw_text_centered_in_rect(buffer, &memory_clock_font_ui_small, widget_x + 28,
                               WIDGET_WIDTH - 56, 304, "to server");
}

static void draw_clock_widget(uint8_t *buffer, const char *weekday,
                              const char *daypart, int hour12, int minute,
                              bool is_pm, const char *date_text,
                              banner_clock_layout_t *layout)
{
    draw_text_centered_in_rect(buffer, &memory_clock_font_weekday, CLOCK_TEXT_X,
                               CLOCK_TEXT_WIDTH, CLOCK_WEEKDAY_Y, weekday);
    draw_text_centered_in_rect(buffer, &memory_clock_font_body, CLOCK_TEXT_X, CLOCK_TEXT_WIDTH,
                               CLOCK_DAYPART_Y, daypart);
    draw_text_centered_in_rect(buffer, &memory_clock_font_date, CLOCK_TEXT_X, CLOCK_TEXT_WIDTH,
                               CLOCK_DATE_Y, date_text);

    char hour_text[3];
    char minute_text[3];
    snprintf(hour_text, sizeof(hour_text), "%d", hour12);
    snprintf(minute_text, sizeof(minute_text), "%02d", minute);

    int hour_field_width = font_measure_text_width(&memory_clock_font_time, "88");
    int colon_width = font_measure_text_width(&memory_clock_font_time, ":");
    int minute_width = font_measure_text_width(&memory_clock_font_time, "88");
    int time_width = hour_field_width + colon_width + minute_width;
    int ampm_width = font_measure_text_width(&memory_clock_font_ui_small, "pm");
    int inline_width = time_width + CLOCK_WIDGET_AMPM_GAP + ampm_width;
    int row_x = CLOCK_TIME_X + (CLOCK_TIME_WIDTH - inline_width) / 2 + CLOCK_TIME_SHIFT_X;

    int hour_width = font_measure_text_width(&memory_clock_font_time, hour_text);
    int hour_x = row_x + hour_field_width - hour_width;
    int colon_x = row_x + hour_field_width;
    int minute_x = colon_x + colon_width;
    int ampm_x = minute_x + minute_width + CLOCK_WIDGET_AMPM_GAP;

    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_time,
                     hour_x, CLOCK_TIME_Y, hour_text, true);
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_time,
                     colon_x, CLOCK_TIME_Y, ":", true);
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_time,
                     minute_x, CLOCK_TIME_Y, minute_text, true);
    font_render_text(buffer, BANNER_WIDTH, BANNER_HEIGHT, &memory_clock_font_ui_small,
                     ampm_x, CLOCK_TIME_Y + 68, is_pm ? "pm" : "am", true);

    if(layout != NULL) {
        memory_clock_rect_t bounds = {0};
        font_measure_text_bounds(&memory_clock_font_time, minute_x, CLOCK_TIME_Y,
                                 "88", &bounds);
        layout->minute_x = bounds.x;
        layout->minute_y = bounds.y;
        layout->minute_width = bounds.width;
        layout->minute_height = bounds.height;
    }
}

void banner_render_status(uint8_t *buffer, size_t buffer_size, const char *headline,
                          const char *detail)
{
    if(buffer_size < BANNER_BUFFER_SIZE) return;

    clear_buffer(buffer, buffer_size);
    draw_logo(buffer, STATUS_LOGO_X, STATUS_LOGO_Y);
    draw_text_centered(buffer, &memory_clock_font_body, STATUS_TITLE_Y, "Memory Clock");
    draw_text_centered(buffer, &memory_clock_font_ui_small, STATUS_HEADLINE_Y, headline);
    draw_text_centered(buffer, &memory_clock_font_ui_small, STATUS_DETAIL_Y, detail);
}

size_t banner_page_count(void)
{
    size_t image_count = image_store_count();
    if(image_count == 0) return 1;
    return 1 + (image_count / 2);
}

void banner_render_page(uint8_t *buffer, size_t buffer_size, size_t page_index,
                        const char *weekday, const char *daypart, int hour12,
                        int minute, bool is_pm, const char *date_text,
                        banner_clock_layout_t *layout)
{
    if(buffer_size < BANNER_BUFFER_SIZE) return;

    clear_buffer(buffer, buffer_size);
    if(layout != NULL) {
        memset(layout, 0, sizeof(*layout));
    }

    image_store_lock();
    if(page_index == 0) {
        draw_clock_widget(buffer, weekday, daypart, hour12, minute, is_pm, date_text, layout);
        image_store_status_t status = image_store_status();
        if(status == IMAGE_STORE_HAS_APPOINTMENTS) {
            draw_image_widget(buffer, RIGHT_WIDGET_X, 0);
        } else if(status == IMAGE_STORE_SERVER_UNAVAILABLE) {
            draw_server_unavailable_widget(buffer, RIGHT_WIDGET_X);
        } else {
            draw_no_appointments_widget(buffer, RIGHT_WIDGET_X);
        }
    } else {
        size_t first_image = 1 + (page_index - 1) * 2;
        draw_image_widget(buffer, 0, first_image);
        draw_image_widget(buffer, RIGHT_WIDGET_X, first_image + 1);
    }
    image_store_unlock();

    draw_rounded_separator(buffer);
}
