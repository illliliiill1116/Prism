#include "synth.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t len, cap;
} wbuf_t;

static void wbuf_init(wbuf_t *w) {
    w->cap = 1 << 20;
    w->data = (uint8_t *)malloc(w->cap);
    w->len = 0;
}

static void wbuf_reserve(wbuf_t *w, size_t extra) {
    if (w->len + extra <= w->cap) return;
    size_t cap = w->cap * 2;
    while (cap < w->len + extra) cap *= 2;
    w->data = (uint8_t *)realloc(w->data, cap);
    w->cap = cap;
}

static void w_u8(wbuf_t *w, uint8_t v) {
    wbuf_reserve(w, 1);
    w->data[w->len++] = v;
}
static void w_u16(wbuf_t *w, uint16_t v) {
    wbuf_reserve(w, 2);
    w->data[w->len++] = (uint8_t)(v >> 8);
    w->data[w->len++] = (uint8_t)v;
}
static void w_u32(wbuf_t *w, uint32_t v) {
    wbuf_reserve(w, 4);
    w->data[w->len++] = (uint8_t)(v >> 24);
    w->data[w->len++] = (uint8_t)(v >> 16);
    w->data[w->len++] = (uint8_t)(v >> 8);
    w->data[w->len++] = (uint8_t)v;
}
static void w_u64(wbuf_t *w, uint64_t v) {
    w_u32(w, (uint32_t)(v >> 32));
    w_u32(w, (uint32_t)v);
}
static void w_bytes(wbuf_t *w, const void *p, size_t n) {
    wbuf_reserve(w, n);
    memcpy(w->data + w->len, p, n);
    w->len += n;
}
static void w_name(wbuf_t *w, const char *name) {
    size_t n = strlen(name);
    w_u16(w, (uint16_t)n);
    w_bytes(w, name, n);
}
static void w_tag_header(wbuf_t *w, uint8_t type, const char *name) {
    w_u8(w, type);
    w_name(w, name);
}
static void w_string(wbuf_t *w, const char *s) {
    size_t n = strlen(s);
    w_u16(w, (uint16_t)n);
    w_bytes(w, s, n);
}

/* xorshift32, deterministic, no external dependency */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Writes a compound body (named children + TAG_End). Used for compounds
 * with a header written by the caller and for compound list elements,
 * which have no header. */
static void w_section(wbuf_t *w, uint32_t *rng, int8_t y) {
    w_tag_header(w, 1, "Y");
    w_u8(w, (uint8_t)y);

    w_tag_header(w, 12 /* long array */, "BlockStates");
    w_u32(w, 256);
    for (int i = 0; i < 256; i++) {
        w_u64(w, ((uint64_t)xorshift32(rng) << 32) | xorshift32(rng));
    }

    w_tag_header(w, 7 /* byte array */, "Biomes");
    w_u32(w, 256);
    for (int i = 0; i < 256; i++) {
        w_u8(w, (uint8_t)(xorshift32(rng) & 0xFF));
    }

    w_u8(w, 0); /* TAG_End of section compound */
}

/* Also just a compound body - see w_section's comment. */
static void w_chunk(wbuf_t *w, uint32_t *rng, int32_t x, int32_t z) {
    w_tag_header(w, 3, "xPos");
    w_u32(w, (uint32_t)x);
    w_tag_header(w, 3, "zPos");
    w_u32(w, (uint32_t)z);
    w_tag_header(w, 4, "LastUpdate");
    w_u64(w, 0x0000019000000000ULL | xorshift32(rng));
    w_tag_header(w, 8, "Status");
    w_string(w, "full");

    w_tag_header(w, 9, "Sections");
    w_u8(w, 10); /* element type: compound */
    w_u32(w, 24); /* 24 sections, y = -4..19 like modern worlds */
    for (int i = 0; i < 24; i++) {
        w_section(w, rng, (int8_t)(i - 4));
    }

    w_u8(w, 0); /* TAG_End of chunk compound */
}

size_t nbt_synth_build_world(uint32_t seed, int num_chunks, uint8_t **out_buf) {
    wbuf_t w;
    wbuf_init(&w);
    uint32_t rng = seed ? seed : 0x9E3779B9u;

    w_tag_header(&w, 10, "level"); /* root compound */

    w_tag_header(&w, 10, "Data");
    w_tag_header(&w, 3, "DataVersion");
    w_u32(&w, 3465);
    w_tag_header(&w, 4, "RandomSeed");
    w_u64(&w, ((uint64_t)xorshift32(&rng) << 32) | xorshift32(&rng));
    w_tag_header(&w, 8, "LevelName");
    w_string(&w, "prism-benchmark-world");
    w_tag_header(&w, 6, "BorderSize");
    { double d = 60000000.0; uint64_t bits; memcpy(&bits, &d, 8); w_u64(&w, bits); }
    w_u8(&w, 0); /* end Data */

    w_tag_header(&w, 9, "Chunks");
    w_u8(&w, 10); /* element type: compound */
    w_u32(&w, (uint32_t)num_chunks);
    for (int i = 0; i < num_chunks; i++) {
        int32_t x = i % 32;
        int32_t z = i / 32;
        w_chunk(&w, &rng, x, z);
    }

    w_u8(&w, 0); /* end root compound */

    *out_buf = w.data;
    return w.len;
}

size_t nbt_synth_build_chunk(uint32_t seed, int32_t x, int32_t z, uint8_t **out_buf) {
    wbuf_t w;
    wbuf_init(&w);
    uint32_t rng = seed ? seed : 0x9E3779B9u;

    /* Modern (1.18+) chunk NBT roots are an unnamed compound. */
    w_u8(&w, 10);
    w_u16(&w, 0);
    w_chunk(&w, &rng, x, z);

    *out_buf = w.data;
    return w.len;
}
