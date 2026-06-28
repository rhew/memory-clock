#include "font_render.h"

#include <stddef.h>
#include <string.h>

static const uint8_t k_bayer_4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static void set_pixel(uint8_t *buffer, int buffer_width, int buffer_height,
                      int x, int y, bool black)
{
    if(x < 0 || x >= buffer_width || y < 0 || y >= buffer_height) return;
    size_t index = (size_t)y * (size_t)buffer_width + (size_t)x;
    uint8_t mask = (uint8_t)(0x80 >> (index & 7));
    if(black) {
        buffer[index / 8] &= (uint8_t)~mask;
    } else {
        buffer[index / 8] |= mask;
    }
}

static const memory_clock_glyph_t *font_find_glyph(const memory_clock_font_t *font, char c)
{
    if(font == NULL) return NULL;
    uint8_t code = (uint8_t)c;
    for(uint16_t i = 0; i < font->glyph_count; ++i) {
        if(font->glyphs[i].codepoint == code) return &font->glyphs[i];
    }
    return NULL;
}

static uint8_t glyph_alpha_at(const memory_clock_glyph_t *glyph, const uint8_t *bitmap_data,
                              int x, int y)
{
    size_t pixel_index = (size_t)y * glyph->width + (size_t)x;
    uint8_t packed = bitmap_data[glyph->bitmap_offset + pixel_index / 2];
    if((pixel_index & 1U) == 0) {
        return (uint8_t)(packed >> 4);
    }
    return (uint8_t)(packed & 0x0f);
}

int font_measure_text_width(const memory_clock_font_t *font, const char *text)
{
    if(font == NULL || text == NULL) return 0;

    int width = 0;
    for(const char *p = text; *p != '\0'; ++p) {
        const memory_clock_glyph_t *glyph = font_find_glyph(font, *p);
        if(glyph != NULL) {
            width += glyph->advance;
        }
    }
    return width;
}

void font_measure_text_bounds(const memory_clock_font_t *font, int x, int y,
                              const char *text, memory_clock_rect_t *bounds)
{
    if(bounds == NULL) return;

    bounds->x = x;
    bounds->y = y;
    bounds->width = 0;
    bounds->height = 0;

    if(font == NULL || text == NULL || *text == '\0') return;

    int baseline = y + font->ascent;
    int pen_x = x;
    int min_x = 0x7fffffff;
    int min_y = 0x7fffffff;
    int max_x = -0x7fffffff;
    int max_y = -0x7fffffff;

    for(const char *p = text; *p != '\0'; ++p) {
        const memory_clock_glyph_t *glyph = font_find_glyph(font, *p);
        if(glyph == NULL) continue;

        int glyph_x = pen_x + glyph->left;
        int glyph_y = baseline - glyph->top;
        if(glyph->width > 0 && glyph->height > 0) {
            if(glyph_x < min_x) min_x = glyph_x;
            if(glyph_y < min_y) min_y = glyph_y;
            if(glyph_x + glyph->width > max_x) max_x = glyph_x + glyph->width;
            if(glyph_y + glyph->height > max_y) max_y = glyph_y + glyph->height;
        }

        pen_x += glyph->advance;
    }

    if(max_x <= min_x || max_y <= min_y) return;

    bounds->x = min_x;
    bounds->y = min_y;
    bounds->width = max_x - min_x;
    bounds->height = max_y - min_y;
}

void font_render_text(uint8_t *buffer, int buffer_width, int buffer_height,
                      const memory_clock_font_t *font, int x, int y,
                      const char *text, bool black)
{
    if(buffer == NULL || font == NULL || text == NULL) return;

    int baseline = y + font->ascent;
    int pen_x = x;

    for(const char *p = text; *p != '\0'; ++p) {
        const memory_clock_glyph_t *glyph = font_find_glyph(font, *p);
        if(glyph == NULL) continue;

        int glyph_x = pen_x + glyph->left;
        int glyph_y = baseline - glyph->top;
        for(int row = 0; row < glyph->height; ++row) {
            for(int col = 0; col < glyph->width; ++col) {
                uint8_t alpha = glyph_alpha_at(glyph, font->bitmap_data, col, row);
                if(alpha == 0) continue;
                if(alpha > k_bayer_4x4[(glyph_y + row) & 3][(glyph_x + col) & 3]) {
                    set_pixel(buffer, buffer_width, buffer_height,
                              glyph_x + col, glyph_y + row, black);
                }
            }
        }

        pen_x += glyph->advance;
    }
}
