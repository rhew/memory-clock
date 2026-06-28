#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct {
    unsigned char codepoint;
    int width;
    int height;
    int left;
    int top;
    int advance;
    size_t bitmap_offset;
} glyph_info_t;

typedef struct {
    const char *name;
    int pixel_size;
    const char *charset;
} font_spec_t;

enum {
    ALPHA_TRIM_THRESHOLD = 48,
};

static const font_spec_t k_fonts[] = {
    {"ui_small", 30,
     " !\"#$%&'()*+,-./0123456789:;?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]abcdefghijklmnopqrstuvwxyz"},
    {"body", 40,
     " !\"#$%&'()*+,-./0123456789:;?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]abcdefghijklmnopqrstuvwxyz"},
    {"date", 34,
     " !\"#$%&'()*+,-./0123456789:;?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]abcdefghijklmnopqrstuvwxyz"},
    {"weekday", 62,
     " !\"#$%&'()*+,-./0123456789:;?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]abcdefghijklmnopqrstuvwxyz"},
    {"time", 120, " 0123456789:amp"},
};

static int append_packed_alpha(uint8_t **data, size_t *size, size_t *capacity, uint8_t alpha)
{
    uint8_t value = (uint8_t)((alpha * 15U + 127U) / 255U);
    if((*size & 1U) == 0) {
        if(*size / 2 >= *capacity) {
            size_t new_capacity = *capacity == 0 ? 256 : *capacity * 2;
            uint8_t *new_data = realloc(*data, new_capacity);
            if(new_data == NULL) return -1;
            *data = new_data;
            *capacity = new_capacity;
        }
        (*data)[*size / 2] = (uint8_t)(value << 4);
    } else {
        (*data)[*size / 2] |= value;
    }
    (*size)++;
    return 0;
}

static int emit_font(FILE *out_h, FILE *out_c, FT_Face face, const font_spec_t *spec)
{
    size_t glyph_count = strlen(spec->charset);
    glyph_info_t *glyphs = calloc(glyph_count, sizeof(*glyphs));
    if(glyphs == NULL) return -1;

    uint8_t *bitmap_data = NULL;
    size_t pixel_count = 0;
    size_t bitmap_capacity = 0;

    if(FT_Set_Pixel_Sizes(face, 0, (FT_UInt)spec->pixel_size) != 0) {
        free(glyphs);
        return -1;
    }

    for(size_t i = 0; i < glyph_count; ++i) {
        unsigned char codepoint = (unsigned char)spec->charset[i];
        if(FT_Load_Char(face, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            free(glyphs);
            free(bitmap_data);
            return -1;
        }

        FT_GlyphSlot slot = face->glyph;
        int source_width = slot->bitmap.width;
        int source_height = slot->bitmap.rows;
        size_t source_pixels = (size_t)source_width * source_height;
        uint8_t *alpha = NULL;
        if(source_pixels > 0) {
            alpha = malloc(source_pixels);
            if(alpha == NULL) {
                free(glyphs);
                free(bitmap_data);
                return -1;
            }
        }

        for(int row = 0; row < source_height; ++row) {
            const uint8_t *src = slot->bitmap.buffer + (size_t)row * slot->bitmap.pitch;
            for(int col = 0; col < source_width; ++col) {
                alpha[(size_t)row * source_width + col] = src[col];
            }
        }

        int min_col = source_width;
        int min_row = source_height;
        int max_col = -1;
        int max_row = -1;
        for(int row = 0; row < source_height; ++row) {
            for(int col = 0; col < source_width; ++col) {
                if(alpha[(size_t)row * source_width + col] > ALPHA_TRIM_THRESHOLD) {
                    if(col < min_col) min_col = col;
                    if(col > max_col) max_col = col;
                    if(row < min_row) min_row = row;
                    if(row > max_row) max_row = row;
                }
            }
        }

        glyphs[i].codepoint = codepoint;
        glyphs[i].advance = (int)(slot->advance.x >> 6);
        glyphs[i].bitmap_offset = pixel_count / 2;

        if(max_col >= min_col && max_row >= min_row) {
            glyphs[i].width = max_col - min_col + 1;
            glyphs[i].height = max_row - min_row + 1;
            glyphs[i].left = slot->bitmap_left + min_col;
            glyphs[i].top = slot->bitmap_top - min_row;

            for(int row = min_row; row <= max_row; ++row) {
                for(int col = min_col; col <= max_col; ++col) {
                    uint8_t sample = alpha[(size_t)row * source_width + col];
                    if(append_packed_alpha(&bitmap_data, &pixel_count, &bitmap_capacity, sample) != 0) {
                        free(alpha);
                        free(glyphs);
                        free(bitmap_data);
                        return -1;
                    }
                }
            }
        } else {
            glyphs[i].width = 0;
            glyphs[i].height = 0;
            glyphs[i].left = 0;
            glyphs[i].top = 0;
        }

        free(alpha);
    }

    size_t packed_size = (pixel_count + 1U) / 2U;
    int ascent = (int)(face->size->metrics.ascender >> 6);
    int line_height = (int)(face->size->metrics.height >> 6);
    if(line_height <= 0) line_height = spec->pixel_size;

    fprintf(out_c, "static const memory_clock_glyph_t %s_glyphs[] = {\n", spec->name);
    for(size_t i = 0; i < glyph_count; ++i) {
        fprintf(out_c,
                "    {0x%02x, %d, %d, %d, %d, %d, %zu},\n",
                glyphs[i].codepoint, glyphs[i].width, glyphs[i].height, glyphs[i].left,
                glyphs[i].top, glyphs[i].advance, glyphs[i].bitmap_offset);
    }
    fprintf(out_c, "};\n\n");

    fprintf(out_c, "static const uint8_t %s_bitmap_data[] = {", spec->name);
    for(size_t i = 0; i < packed_size; ++i) {
        if((i % 12U) == 0) fprintf(out_c, "\n    ");
        fprintf(out_c, "0x%02x,", bitmap_data[i]);
    }
    fprintf(out_c, "\n};\n\n");

    fprintf(out_c,
            "const memory_clock_font_t memory_clock_font_%s = {%d, %d, %zu, %s_glyphs, %s_bitmap_data};\n\n",
            spec->name, line_height, ascent, glyph_count, spec->name, spec->name);

    fprintf(out_h, "extern const memory_clock_font_t memory_clock_font_%s;\n", spec->name);

    free(glyphs);
    free(bitmap_data);
    return 0;
}

int main(int argc, char **argv)
{
    if(argc != 4) {
        fprintf(stderr, "usage: %s FONT.ttf OUT.c OUT.h\n", argv[0]);
        return 1;
    }

    const char *font_path = argv[1];
    const char *out_c_path = argv[2];
    const char *out_h_path = argv[3];

    FT_Library library;
    if(FT_Init_FreeType(&library) != 0) {
        fprintf(stderr, "failed to init FreeType\n");
        return 1;
    }

    FT_Face face;
    if(FT_New_Face(library, font_path, 0, &face) != 0) {
        fprintf(stderr, "failed to load font: %s\n", font_path);
        FT_Done_FreeType(library);
        return 1;
    }

    FILE *out_c = fopen(out_c_path, "w");
    FILE *out_h = fopen(out_h_path, "w");
    if(out_c == NULL || out_h == NULL) {
        fprintf(stderr, "failed to open output: %s\n", strerror(errno));
        if(out_c != NULL) fclose(out_c);
        if(out_h != NULL) fclose(out_h);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return 1;
    }

    fprintf(out_h, "#pragma once\n\n#include <stdint.h>\n\n#include \"font_render.h\"\n\n");
    fprintf(out_c, "#include \"font_assets.h\"\n\n");

    for(size_t i = 0; i < sizeof(k_fonts) / sizeof(k_fonts[0]); ++i) {
        if(emit_font(out_h, out_c, face, &k_fonts[i]) != 0) {
            fprintf(stderr, "failed to emit font %s\n", k_fonts[i].name);
            fclose(out_c);
            fclose(out_h);
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return 1;
        }
    }

    fclose(out_c);
    fclose(out_h);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return 0;
}
