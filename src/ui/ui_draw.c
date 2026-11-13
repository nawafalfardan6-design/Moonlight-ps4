#include <stddef.h>
#include "ui_draw.h"
#include "ui_font.h"



void ui_clear(ui_surface_t *s, uint32_t argb) {
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)y * (size_t)s->pitch);
        for (int x = 0; x < s->w; x++)
            row[x] = argb;
    }
}

void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)(y + r) * (size_t)s->pitch);
        for (int c = 0; c < w; c++)
            row[x + c] = argb;
    }
}


void ui_round_rect(ui_surface_t *s, int x, int y, int w, int h, int radius,
                   uint32_t color) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    ui_rect(s, x, y + radius, w, h - radius * 2, color);
    int inset = radius;
    for (int row = 0; row < radius; row++) {
        int dy = radius - row - 1;
        while (inset && (radius - inset + 1) * (radius - inset + 1) +
                        dy * dy <= radius * radius)
            inset--;
        ui_rect(s, x + inset, y + row, w - 2 * inset, 1, color);
        ui_rect(s, x + inset, y + h - 1 - row, w - 2 * inset, 1, color);
    }
}

static int font_index(int scale) {
    return scale < 1 ? 0 : scale > 5 ? 4 : scale - 1;
}

static const ui_glyph_t *glyph_for(int font, unsigned char c) {
    return &inter_glyphs[font][(c >= 32 && c <= 126 ? c : '?') - 32];
}

static void draw_glyph(ui_surface_t *s, int x, int y, uint32_t color,
                       const ui_glyph_t *g) {
    for (int row = 0; row < g->h; row++) {
        int py = y + g->y + row;
        if (py < 0 || py >= s->h)
            continue;
        uint32_t *dst = (uint32_t *)(void *)(s->fb + (size_t)py * s->pitch);
        for (int col = 0; col < g->w; col++) {
            int px = x + g->x + col;
            if (px < 0 || px >= s->w)
                continue;
            unsigned alpha = inter_coverage[g->offset + row * g->w + col];
            if (!alpha)
                continue;
            uint32_t old = dst[px], pixel = 0xff000000;
            for (unsigned shift = 0; shift < 24; shift += 8) {
                unsigned fg = (color >> shift) & 255;
                unsigned bg = (old >> shift) & 255;
                pixel |= ((fg * alpha + bg * (255 - alpha) + 127) / 255) << shift;
            }
            dst[px] = pixel;
        }
    }
}

void ui_text_fit(ui_surface_t *s, int x, int y, int scale, uint32_t color,
                 int width, const char *str) {
    static const int baselines[] = { 14, 19, 27, 34, 46 };
    int font = font_index(scale);
    int shorten = ui_text_w(scale, str) > width;
    int dots = ui_text_w(scale, "...");
    if (shorten && width < dots)
        return;
    int remaining = shorten ? width - dots : width;
    for (; *str; str++) {
        const ui_glyph_t *g = glyph_for(font, *str);
        if (remaining < g->advance)
            break;
        draw_glyph(s, x, y + baselines[font], color, g);
        x += g->advance;
        remaining -= g->advance;
    }
    if (shorten)
        ui_text_fit(s, x, y, scale, color, dots, "...");
}

void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t color,
             const char *str) {
    ui_text_fit(s, x, y, scale, color, ui_text_w(scale, str), str);
}

int ui_text_w(int scale, const char *str) {
    int font = font_index(scale);
    int width = 0;
    for (; *str; str++)
        width += glyph_for(font, *str)->advance;
    return width;
}
