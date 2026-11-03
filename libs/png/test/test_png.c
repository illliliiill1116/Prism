/* Decoder tests against fixture PNGs covering every bit depth and colour
 * type found in a real client jar: gray 1/8, RGB8, palette 2/4/8 with
 * PLTE+tRNS, gray+alpha 8, RGBA8, and an image using every filter type.
 * The fixtures were built independently of this encoder (Python's zlib),
 * so this isn't a round trip through our own code. FIXTURES_DIR comes from
 * the Makefile as an absolute path. */
#include "png_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIXTURES_DIR
#define FIXTURES_DIR "test/fixtures"
#endif

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static char *fixture_path(const char *name) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/%s", FIXTURES_DIR, name);
    return buf;
}

static void check_pixel(const canvas_t *img, uint32_t x, uint32_t y,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char *msg) {
    const uint8_t *p = img->pixels + ((size_t)y * img->width + x) * 4;
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        fprintf(stderr, "FAIL: %s: pixel(%u,%u) got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
                msg, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        failures++;
    }
}

static void test_rgba8(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("rgba8.png"), &img);
    CHECK(r == PNG_OK, "rgba8.png decodes");
    if (r != PNG_OK) return;
    CHECK(img.width == 4 && img.height == 2, "rgba8.png has expected dimensions");
    check_pixel(&img, 0, 0, 255, 0, 0, 255, "rgba8 pixel 0,0");
    check_pixel(&img, 2, 0, 0, 0, 255, 128, "rgba8 pixel 2,0 (partial alpha)");
    check_pixel(&img, 3, 0, 255, 255, 0, 0, "rgba8 pixel 3,0 (zero alpha)");
    check_pixel(&img, 3, 1, 0, 0, 0, 0, "rgba8 pixel 3,1 (fully transparent black)");
    canvas_free(&img);
}

static void test_rgb8(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("rgb8.png"), &img);
    CHECK(r == PNG_OK, "rgb8.png decodes");
    if (r != PNG_OK) return;
    for (uint32_t i = 0; i < img.width * img.height; i++) {
        uint32_t x = i % img.width, y = i / img.width;
        uint8_t expect_r = (uint8_t)(i * 17 % 256), expect_g = (uint8_t)(i * 31 % 256), expect_b = (uint8_t)(i * 53 % 256);
        check_pixel(&img, x, y, expect_r, expect_g, expect_b, 255, "rgb8 pixel (opaque, no alpha channel)");
    }
    canvas_free(&img);
}

static void test_gray8(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("gray8.png"), &img);
    CHECK(r == PNG_OK, "gray8.png decodes");
    if (r != PNG_OK) return;
    static const uint8_t vals[8] = {0, 64, 128, 192, 255, 32, 96, 160};
    for (uint32_t i = 0; i < 8; i++) {
        check_pixel(&img, i % 4, i / 4, vals[i], vals[i], vals[i], 255, "gray8 pixel (r==g==b==gray value)");
    }
    canvas_free(&img);
}

static void test_gray1(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("gray1.png"), &img);
    CHECK(r == PNG_OK, "gray1.png (1-bit, MSB-first packing) decodes");
    if (r != PNG_OK) return;
    static const int bits[8] = {1, 0, 1, 1, 0, 0, 1, 0};
    for (uint32_t i = 0; i < 8; i++) {
        uint8_t v = bits[i] ? 255 : 0;
        check_pixel(&img, i, 0, v, v, v, 255, "gray1 pixel (1-bit sample scaled to 0/255)");
    }
    canvas_free(&img);
}

static void test_gray_alpha8(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("gray_alpha8.png"), &img);
    CHECK(r == PNG_OK, "gray_alpha8.png decodes");
    if (r != PNG_OK) return;
    static const uint8_t ga[4][2] = {{10, 255}, {50, 200}, {90, 100}, {255, 0}};
    for (uint32_t i = 0; i < 4; i++) {
        check_pixel(&img, i, 0, ga[i][0], ga[i][0], ga[i][0], ga[i][1], "gray_alpha8 pixel");
    }
    canvas_free(&img);
}

static void test_palette8(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("palette8.png"), &img);
    CHECK(r == PNG_OK, "palette8.png (with PLTE+tRNS) decodes");
    if (r != PNG_OK) return;
    static const uint8_t plte[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    static const uint8_t trns[4] = {255, 128, 0, 255};
    static const int indices[8] = {0, 1, 2, 3, 3, 2, 1, 0};
    for (uint32_t i = 0; i < 8; i++) {
        int idx = indices[i];
        check_pixel(&img, i % 4, i / 4, plte[idx][0], plte[idx][1], plte[idx][2], trns[idx],
                    "palette8 pixel (PLTE color + per-entry tRNS alpha)");
    }
    canvas_free(&img);
}

static void test_palette4(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("palette4.png"), &img);
    CHECK(r == PNG_OK, "palette4.png (4-bit packed indices, no tRNS) decodes");
    if (r != PNG_OK) return;
    for (uint32_t i = 0; i < 8; i++) {
        uint8_t expect_r = (uint8_t)(i * 16 % 256), expect_g = (uint8_t)(i * 32 % 256), expect_b = (uint8_t)(i * 48 % 256);
        check_pixel(&img, i, 0, expect_r, expect_g, expect_b, 255, "palette4 pixel (default opaque, no tRNS)");
    }
    canvas_free(&img);
}

static void test_palette2(void) {
    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("palette2.png"), &img);
    CHECK(r == PNG_OK, "palette2.png (2-bit packed indices, 4 per byte) decodes");
    if (r != PNG_OK) return;
    static const uint8_t plte[4][3] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};
    static const int indices[8] = {0, 1, 2, 3, 3, 2, 1, 0};
    for (uint32_t i = 0; i < 8; i++) {
        int idx = indices[i];
        check_pixel(&img, i, 0, plte[idx][0], plte[idx][1], plte[idx][2], 255, "palette2 pixel");
    }
    canvas_free(&img);
}

static void test_all_filter_types(void) {
    FILE *f = fopen(fixture_path("rgb8_filters_expected.txt"), "r");
    CHECK(f != NULL, "expected-pixels fixture opens");
    if (!f) return;

    canvas_t img;
    png_result_t r = png_decode_file(fixture_path("rgb8_filters.png"), &img);
    CHECK(r == PNG_OK, "rgb8_filters.png (exercises None/Sub/Up/Average/Paeth) decodes");
    if (r == PNG_OK) {
        char line[256];
        uint32_t y = 0;
        while (fgets(line, sizeof(line), f) && y < img.height) {
            char *tok = strtok(line, " \n");
            uint32_t x = 0;
            while (tok && x < img.width) {
                int rr, gg, bb;
                sscanf(tok, "%d,%d,%d", &rr, &gg, &bb);
                check_pixel(&img, x, y, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb, 255, "filtered rgb8 pixel");
                tok = strtok(NULL, " \n");
                x++;
            }
            y++;
        }
        canvas_free(&img);
    }
    fclose(f);
}

static void test_encode_decode_round_trip(void) {
    canvas_t img;
    img.width = 3;
    img.height = 2;
    img.stride = 3 * 4;
    img.owns_pixels = 0; /* freed manually below, not via canvas_free */
    img.pixels = (uint8_t *)malloc(3 * 2 * 4);
    for (int i = 0; i < 3 * 2; i++) {
        img.pixels[i * 4 + 0] = (uint8_t)(i * 40);
        img.pixels[i * 4 + 1] = (uint8_t)(i * 20);
        img.pixels[i * 4 + 2] = (uint8_t)(i * 10);
        img.pixels[i * 4 + 3] = (uint8_t)(255 - i * 30);
    }

    const char *path = "/tmp/prism_png_roundtrip_test.png";
    png_result_t wr = png_encode_to_file(path, &img);
    CHECK(wr == PNG_OK, "round-trip image encodes");

    canvas_t decoded;
    png_result_t rr = png_decode_file(path, &decoded);
    CHECK(rr == PNG_OK, "round-trip image decodes back");
    if (rr == PNG_OK) {
        CHECK(decoded.width == img.width && decoded.height == img.height, "round-trip dimensions match");
        int mismatches = 0;
        for (size_t i = 0; i < (size_t)img.width * img.height * 4; i++) {
            if (decoded.pixels[i] != img.pixels[i]) mismatches++;
        }
        CHECK(mismatches == 0, "round-trip pixels match exactly (lossless, filter type None)");
        canvas_free(&decoded);
    }

    remove(path);
    free(img.pixels);
}

int main(void) {
    test_rgba8();
    test_rgb8();
    test_gray8();
    test_gray1();
    test_gray_alpha8();
    test_palette8();
    test_palette4();
    test_palette2();
    test_all_filter_types();
    test_encode_decode_round_trip();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
