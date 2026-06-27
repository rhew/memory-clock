#include "banner.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "logo_bits.xbm"

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

static const uint8_t *glyph_for(char c)
{
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t dash[7] = {0, 0, 0, 0x1f, 0, 0, 0};
    static const uint8_t comma[7] = {0, 0, 0, 0, 0, 0x0c, 0x08};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 0x0c, 0x0c};
    static const uint8_t colon[7] = {0, 0x0c, 0x0c, 0, 0x0c, 0x0c, 0};
    static const uint8_t one[7] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t two[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
    static const uint8_t three[7] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t four[7] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
    static const uint8_t five[7] = {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e};
    static const uint8_t six[7] = {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e};
    static const uint8_t seven[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const uint8_t eight[7] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
    static const uint8_t nine[7] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c};
    static const uint8_t zero[7] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
    static const uint8_t A[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t B[7] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
    static const uint8_t C[7] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
    static const uint8_t D[7] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
    static const uint8_t E[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    static const uint8_t F[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
    static const uint8_t G[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f};
    static const uint8_t H[7] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t I[7] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t J[7] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e};
    static const uint8_t K[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t M[7] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t O[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t P[7] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
    static const uint8_t R[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const uint8_t S[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t T[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t U[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t W[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};
    static const uint8_t Y[7] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t a[7] = {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f};
    static const uint8_t b[7] = {0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x1e};
    static const uint8_t c_lower[7] = {0x00, 0x00, 0x0e, 0x11, 0x10, 0x11, 0x0e};
    static const uint8_t d[7] = {0x01, 0x01, 0x0f, 0x11, 0x11, 0x11, 0x0f};
    static const uint8_t e[7] = {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e};
    static const uint8_t f[7] = {0x06, 0x08, 0x1c, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t g[7] = {0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e};
    static const uint8_t h[7] = {0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t i[7] = {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t j[7] = {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c};
    static const uint8_t k[7] = {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12};
    static const uint8_t l[7] = {0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t m[7] = {0x00, 0x00, 0x1a, 0x15, 0x15, 0x15, 0x15};
    static const uint8_t n[7] = {0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t o[7] = {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t p[7] = {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10};
    static const uint8_t r[7] = {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10};
    static const uint8_t s[7] = {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e};
    static const uint8_t t[7] = {0x08, 0x08, 0x1c, 0x08, 0x08, 0x08, 0x06};
    static const uint8_t u[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d};
    static const uint8_t v[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04};
    static const uint8_t w[7] = {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a};
    static const uint8_t y[7] = {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e};

    switch(c) {
    case '0': return zero;
    case '1': return one;
    case '2': return two;
    case '3': return three;
    case '4': return four;
    case '5': return five;
    case '6': return six;
    case '7': return seven;
    case '8': return eight;
    case '9': return nine;
    case 'A': return A;
    case 'B': return B;
    case 'C': return C;
    case 'D': return D;
    case 'E': return E;
    case 'F': return F;
    case 'G': return G;
    case 'H': return H;
    case 'I': return I;
    case 'J': return J;
    case 'K': return K;
    case 'L': return L;
    case 'M': return M;
    case 'N': return N;
    case 'O': return O;
    case 'P': return P;
    case 'R': return R;
    case 'S': return S;
    case 'T': return T;
    case 'U': return U;
    case 'W': return W;
    case 'Y': return Y;
    case 'a': return a;
    case 'b': return b;
    case 'c': return c_lower;
    case 'd': return d;
    case 'e': return e;
    case 'f': return f;
    case 'g': return g;
    case 'h': return h;
    case 'i': return i;
    case 'j': return j;
    case 'k': return k;
    case 'l': return l;
    case 'm': return m;
    case 'n': return n;
    case 'o': return o;
    case 'p': return p;
    case 'r': return r;
    case 's': return s;
    case 't': return t;
    case 'u': return u;
    case 'v': return v;
    case 'w': return w;
    case 'y': return y;
    case '-': return dash;
    case ',': return comma;
    case '.': return dot;
    case ':': return colon;
    default: return blank;
    }
}

static void draw_char(uint8_t *buffer, int x, int y, char c, int scale)
{
    const uint8_t *glyph = glyph_for(c);
    for(int row = 0; row < 7; ++row) {
        for(int col = 0; col < 5; ++col) {
            if((glyph[row] & (1 << (4 - col))) != 0) {
                for(int dy = 0; dy < scale; ++dy) {
                    for(int dx = 0; dx < scale; ++dx) {
                        set_pixel(buffer, x + col * scale + dx, y + row * scale + dy, true);
                    }
                }
            }
        }
    }
}

static void draw_text(uint8_t *buffer, int x, int y, const char *text, int scale)
{
    int cursor = x;
    int advance = 6 * scale;
    for(const char *p = text; *p != '\0'; ++p) {
        draw_char(buffer, cursor, y, *p, scale);
        cursor += advance;
    }
}

static int text_width(const char *text, int scale)
{
    int count = 0;
    for(const char *p = text; *p != '\0'; ++p) ++count;
    if(count == 0) return 0;
    return ((count - 1) * 6 + 5) * scale;
}

static void draw_text_centered(uint8_t *buffer, int y, const char *text, int scale)
{
    int width = text_width(text, scale);
    int x = (BANNER_WIDTH - width) / 2;
    draw_text(buffer, x, y, text, scale);
}

static void clear_buffer(uint8_t *buffer, size_t buffer_size)
{
    memset(buffer, 0xff, buffer_size);
}

void banner_render_status(uint8_t *buffer, size_t buffer_size, const char *headline,
                          const char *detail)
{
    if(buffer_size < BANNER_BUFFER_SIZE) return;
    clear_buffer(buffer, buffer_size);

    draw_logo(buffer, 352, 64);
    draw_text_centered(buffer, 190, "Memory Clock", 6);
    draw_text_centered(buffer, 286, headline, 3);
    draw_text_centered(buffer, 338, detail, 3);
}

void banner_render_clock(uint8_t *buffer, size_t buffer_size, const char *weekday,
                         const char *daypart, int hour12, int minute, bool is_pm,
                         const char *date_text)
{
    if(buffer_size < BANNER_BUFFER_SIZE) return;

    clear_buffer(buffer, buffer_size);

    char time_text[6];
    snprintf(time_text, sizeof(time_text), "%d:%02d", hour12, minute);
    const char *ampm = is_pm ? "pm" : "am";

    const int weekday_scale = 7;
    const int daypart_scale = 5;
    const int time_scale = 20;
    const int ampm_scale = 8;
    const int date_scale = 5;
    const int gap = 20;
    const int weekday_y = 34;
    const int daypart_y = 102;
    const int time_y = 176;
    const int ampm_y = time_y + 84;
    const int date_y = 380;

    int time_width_px = text_width(time_text, time_scale);
    int ampm_width_px = text_width(ampm, ampm_scale);
    int total_width_px = time_width_px + gap + ampm_width_px;
    int time_x = (BANNER_WIDTH - total_width_px) / 2;
    int ampm_x = time_x + time_width_px + gap;

    draw_text_centered(buffer, weekday_y, weekday, weekday_scale);
    draw_text_centered(buffer, daypart_y, daypart, daypart_scale);
    draw_text(buffer, time_x, time_y, time_text, time_scale);
    draw_text(buffer, ampm_x, ampm_y, ampm, ampm_scale);
    draw_text_centered(buffer, date_y, date_text, date_scale);
}
