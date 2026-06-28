#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t codepoint;
    uint8_t width;
    uint8_t height;
    int8_t left;
    int8_t top;
    uint8_t advance;
    uint32_t bitmap_offset;
} memory_clock_glyph_t;

typedef struct {
    uint8_t line_height;
    uint8_t ascent;
    uint16_t glyph_count;
    const memory_clock_glyph_t *glyphs;
    const uint8_t *bitmap_data;
} memory_clock_font_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} memory_clock_rect_t;

void font_render_text(uint8_t *buffer, int buffer_width, int buffer_height,
                      const memory_clock_font_t *font, int x, int y,
                      const char *text, bool black);
int font_measure_text_width(const memory_clock_font_t *font, const char *text);
void font_measure_text_bounds(const memory_clock_font_t *font, int x, int y,
                              const char *text, memory_clock_rect_t *bounds);
