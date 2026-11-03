/*
 * canvas.h - the RGBA8 pixel buffer every layer shares: decoded textures,
 * cached cutout textures and render framebuffers. Codecs read and write
 * canvases; nothing below them knows a file format.
 *
 * `stride` is independent of `width` so a canvas can be a zero-copy view
 * into part of a larger one (canvas_subview).
 */
#ifndef PRISM_CANVAS_H
#define PRISM_CANVAS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct canvas_st {
    uint32_t width, height;
    ptrdiff_t stride; /* bytes between the start of one row and the next, >= width*4 */
    uint8_t *pixels;  /* top-left origin, row-major RGBA8; NULL on failure/empty */
    int owns_pixels;  /* internal - set by canvas_create, cleared by canvas_subview; see canvas_free */
} canvas_t;

/* An owned, tightly packed canvas (stride == width*4), zeroed. .pixels is
 * NULL on OOM or a zero size - check it. */
canvas_t canvas_create(uint32_t width, uint32_t height);

/* Frees an owned canvas. A no-op on a view or an already freed/zeroed
 * canvas. */
void canvas_free(canvas_t *c);

/* Zero-copy view of [x, x+width) x [y, y+height) of `parent`, clamped to
 * its bounds; never owns memory. A rect wholly outside gives width ==
 * height == 0 (pixels may be non-NULL) - check the size, not pixels. */
canvas_t canvas_subview(const canvas_t *parent, int32_t x, int32_t y, uint32_t width, uint32_t height);

/* Byte offset of a pixel, honouring stride. */
static inline uint8_t *canvas_row(const canvas_t *c, uint32_t y)
{
    return c->pixels + (size_t)y * (size_t)c->stride;
}

/* Fills the whole canvas with one solid color (respects stride, so this
 * is also correct on a subview). */
void canvas_fill(canvas_t *c, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* "Over" compositing of one pixel; out-of-bounds writes are clipped. */
void canvas_blend_pixel(canvas_t *c, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* The blend on a raw pixel pointer, with no bounds check, for loops that
 * clipped their row already. The destination is assumed opaque, so the
 * output alpha is always 255. */
static inline void canvas_blend_px(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0) return;
    if (a == 255) {
        dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = 255;
        return;
    }
    dst[0] = (uint8_t)((r * a + dst[0] * (255 - a)) / 255);
    dst[1] = (uint8_t)((g * a + dst[1] * (255 - a)) / 255);
    dst[2] = (uint8_t)((b * a + dst[2] * (255 - a)) / 255);
    dst[3] = 255;
}

#ifdef __cplusplus
}
#endif

#endif /* PRISM_CANVAS_H */
