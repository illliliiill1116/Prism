#include "canvas.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static uint8_t *px(const canvas_t *c, uint32_t x, uint32_t y) {
    return canvas_row(c, y) + (size_t)x * 4;
}

int main(void) {
    canvas_t c = canvas_create(4, 3);
    CHECK(c.pixels != NULL, "canvas_create succeeds");
    CHECK(c.width == 4 && c.height == 3, "canvas_create dimensions");
    CHECK(c.stride == 16, "canvas_create is tightly packed (stride == width*4)");

    canvas_fill(&c, 10, 20, 30, 255);
    uint8_t *p = px(&c, 2, 1);
    CHECK(p[0] == 10 && p[1] == 20 && p[2] == 30 && p[3] == 255, "canvas_fill sets every pixel");

    canvas_blend_pixel(&c, 2, 1, 255, 0, 0, 255);
    p = px(&c, 2, 1);
    CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0, "opaque blend overwrites");

    canvas_blend_pixel(&c, 2, 1, 0, 0, 0, 0);
    p = px(&c, 2, 1);
    CHECK(p[0] == 255, "alpha=0 blend is a no-op");

    canvas_blend_pixel(&c, -1, 0, 1, 2, 3, 255);
    canvas_blend_pixel(&c, 100, 0, 1, 2, 3, 255);
    canvas_blend_pixel(&c, 0, 100, 1, 2, 3, 255);
    CHECK(1, "out-of-bounds blends don't crash");

    /* subview: a 2x2 window at (1,1) shares memory with the parent */
    canvas_t v = canvas_subview(&c, 1, 1, 2, 2);
    CHECK(v.width == 2 && v.height == 2, "subview dimensions");
    CHECK(v.stride == c.stride, "subview keeps parent's stride");
    canvas_blend_pixel(&v, 0, 0, 9, 9, 9, 255);
    p = px(&c, 1, 1);
    CHECK(p[0] == 9 && p[1] == 9 && p[2] == 9, "subview writes land on the parent buffer");

    /* subview clamped to parent bounds */
    canvas_t clipped = canvas_subview(&c, 3, 2, 5, 5);
    CHECK(clipped.width == 1 && clipped.height == 1, "subview clamps to parent bounds");

    canvas_t empty = canvas_subview(&c, 10, 10, 2, 2);
    CHECK(empty.width == 0 && empty.height == 0, "fully out-of-range subview is empty");

    canvas_free(&c);
    CHECK(c.pixels == NULL, "canvas_free clears pixels");
    canvas_free(&v); /* no-op on a view: must not double-free */

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all canvas tests passed\n");
    return 0;
}
