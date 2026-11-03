#include "synth_section.h"
#include "chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t len, cap;
} wbuf_t;

static void wbuf_init(wbuf_t *w) {
    w->cap = 1 << 16;
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
static void w_u8(wbuf_t *w, uint8_t v) { wbuf_reserve(w, 1); w->data[w->len++] = v; }
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
static void w_u64(wbuf_t *w, uint64_t v) { w_u32(w, (uint32_t)(v >> 32)); w_u32(w, (uint32_t)v); }
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
static void w_tag_header(wbuf_t *w, uint8_t type, const char *name) { w_u8(w, type); w_name(w, name); }
static void w_string_payload(wbuf_t *w, const char *s) {
    size_t n = strlen(s);
    w_u16(w, (uint16_t)n);
    w_bytes(w, s, n);
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t bits_needed(uint32_t n) {
    uint32_t bits = 0, v = 1;
    while (v < n) { v <<= 1; bits++; }
    return bits;
}

/* Packs `count` values (< 1<<bits each) without straddling longs, as a
 * length-prefixed big-endian TAG_Long_Array payload. */
static void w_packed_long_array(wbuf_t *w, const uint32_t *values, uint32_t count, uint32_t bits) {
    uint32_t values_per_long = 64u / bits;
    uint32_t num_longs = (count + values_per_long - 1) / values_per_long;
    w_u32(w, (int32_t)num_longs);
    uint32_t idx = 0;
    for (uint32_t li = 0; li < num_longs; li++) {
        uint64_t word = 0;
        for (uint32_t j = 0; j < values_per_long && idx < count; j++, idx++) {
            word |= (uint64_t)values[idx] << (j * bits);
        }
        w_u64(w, word);
    }
}

/* Writes a paletted-container compound body (palette list plus optional
 * data) holding exactly `indices[count]`. `names` gives `palette_size`
 * names; `entry_is_compound` picks {Name: ...} entries (block_states) or
 * bare strings (biomes). `min_bits` is 4 or 0 respectively. */
static void w_paletted_container(wbuf_t *w, const char *const *names,
                                 uint32_t palette_size, int entry_is_compound,
                                 uint32_t min_bits, uint32_t count, const uint32_t *indices) {
    w_tag_header(w, 9, "palette");
    w_u8(w, entry_is_compound ? 10 : 8);
    w_u32(w, (int32_t)palette_size);
    for (uint32_t i = 0; i < palette_size; i++) {
        if (entry_is_compound) {
            w_tag_header(w, 8, "Name");
            w_string_payload(w, names[i]);
            w_u8(w, 0); /* TAG_End of palette entry compound */
        } else {
            w_string_payload(w, names[i]);
        }
    }

    if (palette_size > 1) {
        uint32_t bits = bits_needed(palette_size);
        if (bits < min_bits) bits = min_bits;

        w_tag_header(w, 12, "data");
        w_packed_long_array(w, indices, count, bits);
    }
}

static void w_section(wbuf_t *w, uint32_t *rng, int8_t y, uint32_t block_palette_size) {
    w_tag_header(w, 1, "Y");
    w_u8(w, (uint8_t)y);

    char names_buf[64][24];
    const char *names[64];
    for (uint32_t i = 0; i < block_palette_size; i++) {
        snprintf(names_buf[i], sizeof(names_buf[i]), "minecraft:block_%u", i);
        names[i] = names_buf[i];
    }

    uint32_t block_indices[CHUNK_BLOCKS_PER_SECTION];
    for (uint32_t i = 0; i < CHUNK_BLOCKS_PER_SECTION; i++) {
        block_indices[i] = xorshift32(rng) % block_palette_size;
    }
    w_tag_header(w, 10, "block_states");
    w_paletted_container(w, names, block_palette_size, 1, 4, CHUNK_BLOCKS_PER_SECTION, block_indices);
    w_u8(w, 0); /* end block_states */

    static const char *biome_names[4] = {
        "minecraft:plains", "minecraft:forest", "minecraft:desert", "minecraft:ocean",
    };
    uint32_t biome_indices[CHUNK_BIOMES_PER_SECTION];
    for (uint32_t i = 0; i < CHUNK_BIOMES_PER_SECTION; i++) {
        biome_indices[i] = xorshift32(rng) % 4;
    }
    w_tag_header(w, 10, "biomes");
    w_paletted_container(w, biome_names, 4, 0, 0, CHUNK_BIOMES_PER_SECTION, biome_indices);
    w_u8(w, 0); /* end biomes */

    w_u8(w, 0); /* end section compound */
}

size_t chunk_synth_build_chunk(uint32_t seed, int32_t x, int32_t z,
                               int num_sections, uint32_t block_palette_size,
                               uint8_t **out_buf) {
    wbuf_t w;
    wbuf_init(&w);
    uint32_t rng = seed ? seed : 0x9E3779B9u;
    if (block_palette_size > 64) block_palette_size = 64;
    if (block_palette_size < 1) block_palette_size = 1;

    w_u8(&w, 10); /* root: unnamed compound */
    w_u16(&w, 0);

    w_tag_header(&w, 3, "xPos");
    w_u32(&w, (uint32_t)x);
    w_tag_header(&w, 3, "zPos");
    w_u32(&w, (uint32_t)z);
    w_tag_header(&w, 8, "Status");
    w_string_payload(&w, "full");

    w_tag_header(&w, 9, "sections");
    w_u8(&w, 10);
    w_u32(&w, num_sections);
    for (int i = 0; i < num_sections; i++) {
        w_section(&w, &rng, (int8_t)(i - 4), block_palette_size);
    }

    w_u8(&w, 0); /* end root compound */

    *out_buf = w.data;
    return w.len;
}

size_t chunk_synth_build_container_doc(const char *container_key,
                                       const char *const *names, uint32_t palette_size,
                                       int entry_is_compound, uint32_t min_bits,
                                       uint32_t count, const uint32_t *indices,
                                       uint8_t **out_buf) {
    wbuf_t w;
    wbuf_init(&w);

    w_u8(&w, 10); /* root: unnamed compound */
    w_u16(&w, 0);

    w_tag_header(&w, 10, container_key);
    w_paletted_container(&w, names, palette_size, entry_is_compound, min_bits, count, indices);
    w_u8(&w, 0); /* end container_key compound */

    w_u8(&w, 0); /* end root compound */

    *out_buf = w.data;
    return w.len;
}
