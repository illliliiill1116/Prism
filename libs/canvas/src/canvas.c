#include "canvas.h"

#include <stdlib.h>
#include <string.h>

canvas_t canvas_create(uint32_t width, uint32_t height) {
    canvas_t c = {0, 0, 0, NULL, 0};
    if (width == 0 || height == 0) return c;
    uint8_t *pixels = (uint8_t *)calloc((size_t)width * height, 4);
    if (!pixels) return c;
    c.width = width;
    c.height = height;
    c.stride = (ptrdiff_t)width * 4;
    c.pixels = pixels;
    c.owns_pixels = 1;
    return c;
}

void canvas_free(canvas_t *c) {
    if (!c) return;
    if (c->owns_pixels) free(c->pixels);
    c->pixels = NULL;
    c->width = c->height = 0;
    c->stride = 0;
    c->owns_pixels = 0;
}

canvas_t canvas_subview(const canvas_t *parent, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    canvas_t v = {0, 0, 0, parent->pixels, 0};
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + width, y1 = (int64_t)y + height;
    if (x1 > (int64_t)parent->width) x1 = parent->width;
    if (y1 > (int64_t)parent->height) y1 = parent->height;
    if (x1 <= x0 || y1 <= y0) { v.width = v.height = 0; return v; }

    v.width = (uint32_t)(x1 - x0);
    v.height = (uint32_t)(y1 - y0);
    v.stride = parent->stride;
    v.pixels = parent->pixels + (size_t)y0 * parent->stride + (size_t)x0 * 4;
    return v;
}

void canvas_fill(canvas_t *c, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (c->width == 0 || c->height == 0) return;

    /* Build the first row pixel by pixel, then memcpy it over the others:
     * much faster than byte stores for large fills, and row by row so a
     * strided view works. */
    uint8_t *first = canvas_row(c, 0);
    for (uint32_t x = 0; x < c->width; x++) {
        uint8_t *p = first + (size_t)x * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
    size_t row_bytes = (size_t)c->width * 4;
    for (uint32_t y = 1; y < c->height; y++)
        memcpy(canvas_row(c, y), first, row_bytes);
}

void canvas_blend_pixel(canvas_t *c, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || (uint32_t)x >= c->width || (uint32_t)y >= c->height) return;
    canvas_blend_px(canvas_row(c, (uint32_t)y) + (size_t)x * 4, r, g, b, a);
}
