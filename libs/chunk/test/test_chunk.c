/* Paletted-container decoding against synthetic NBT. Round-trip tests pack
 * chosen indices (chunk_synth_build_container_doc) and compare exactly; the
 * chunk_synth_build_chunk test checks invariants (ranges, names, counts). */
#include "chunk.h"
#include "nbt.h"
#include "synth_section.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static void test_single_entry_palette(void) {
    const char *names[1] = { "minecraft:stone" };
    uint32_t indices[CHUNK_BLOCKS_PER_SECTION] = {0}; /* unused: single-entry has no data array */

    uint8_t *buf;
    size_t len = chunk_synth_build_container_doc("block_states", names, 1, 1, 4,
                                                  CHUNK_BLOCKS_PER_SECTION, indices, &buf);

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    nbt_result_t pr = nbt_parse(&pool, buf, len, &root);
    CHECK(pr == NBT_OK, "single-entry-palette doc parses");

    nbt_tag_t *container = nbt_compound_getz(&pool, root, "block_states");
    CHECK(container != NULL, "block_states compound present");

    uint16_t out[CHUNK_BLOCKS_PER_SECTION];
    const nbt_tag_t *palette;
    /* The document's root is the container itself, so call the low-level
     * decoder rather than chunk_decode_block_states (which looks it up in a
     * section). */
    chunk_result_t cr = chunk_decode_paletted_container(&pool, container, CHUNK_BLOCKS_PER_SECTION, 4, out, &palette);
    CHECK(cr == CHUNK_OK, "decode succeeds with a single-entry palette");
    CHECK(palette->child_count == 1, "palette has exactly one entry");

    int all_zero = 1;
    for (uint32_t i = 0; i < CHUNK_BLOCKS_PER_SECTION; i++) if (out[i] != 0) all_zero = 0;
    CHECK(all_zero, "every cell is implicitly index 0 when the palette has one entry");

    const char *name; uint16_t name_len;
    chunk_result_t nr = chunk_palette_entry_name(&pool, palette, 0, &name, &name_len);
    CHECK(nr == CHUNK_OK && name_len == strlen("minecraft:stone") &&
          memcmp(name, "minecraft:stone", name_len) == 0, "palette entry 0 name is 'minecraft:stone'");

    nbt_pool_free(&pool);
    free(buf);
}

static void test_exact_round_trip_block_states(void) {
    const char *names[6] = {
        "minecraft:air", "minecraft:stone", "minecraft:dirt",
        "minecraft:grass_block", "minecraft:water", "minecraft:bedrock",
    };
    uint32_t palette_size = 6; /* bits_needed(6) == 3, forced up to min_bits=4 */

    uint32_t indices[CHUNK_BLOCKS_PER_SECTION];
    uint32_t rng = 777;
    for (uint32_t i = 0; i < CHUNK_BLOCKS_PER_SECTION; i++) {
        rng = rng * 1103515245u + 12345u;
        indices[i] = (rng >> 16) % palette_size;
    }

    uint8_t *buf;
    size_t len = chunk_synth_build_container_doc("block_states", names, palette_size, 1, 4,
                                                  CHUNK_BLOCKS_PER_SECTION, indices, &buf);

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    CHECK(nbt_parse(&pool, buf, len, &root) == NBT_OK, "6-entry-palette doc parses");

    nbt_tag_t *container = nbt_compound_getz(&pool, root, "block_states");
    uint16_t out[CHUNK_BLOCKS_PER_SECTION];
    const nbt_tag_t *palette;
    chunk_result_t cr = chunk_decode_paletted_container(&pool, container, CHUNK_BLOCKS_PER_SECTION, 4, out, &palette);
    CHECK(cr == CHUNK_OK, "decode succeeds with a 6-entry palette (forced to 4 bits)");
    CHECK(palette->child_count == palette_size, "palette length matches what was written");

    int mismatches = 0;
    for (uint32_t i = 0; i < CHUNK_BLOCKS_PER_SECTION; i++) {
        if (out[i] != indices[i]) mismatches++;
    }
    CHECK(mismatches == 0, "every decoded index exactly matches what was packed");

    for (uint32_t p = 0; p < palette_size; p++) {
        const char *name; uint16_t name_len;
        chunk_result_t nr = chunk_palette_entry_name(&pool, palette, p, &name, &name_len);
        CHECK(nr == CHUNK_OK && name_len == strlen(names[p]) && memcmp(name, names[p], name_len) == 0,
              "palette entry name round-trips");
    }

    nbt_pool_free(&pool);
    free(buf);
}

static void test_exact_round_trip_biomes(void) {
    const char *names[3] = { "minecraft:plains", "minecraft:desert", "minecraft:ocean" };
    uint32_t palette_size = 3; /* bits_needed(3) == 2, min_bits=0 so stays at 2 */

    uint32_t indices[CHUNK_BIOMES_PER_SECTION];
    for (uint32_t i = 0; i < CHUNK_BIOMES_PER_SECTION; i++) indices[i] = i % palette_size;

    uint8_t *buf;
    size_t len = chunk_synth_build_container_doc("biomes", names, palette_size, 0, 0,
                                                  CHUNK_BIOMES_PER_SECTION, indices, &buf);

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    CHECK(nbt_parse(&pool, buf, len, &root) == NBT_OK, "biomes doc parses");

    nbt_tag_t *container = nbt_compound_getz(&pool, root, "biomes");
    uint16_t out[CHUNK_BIOMES_PER_SECTION];
    const nbt_tag_t *palette;
    chunk_result_t cr = chunk_decode_paletted_container(&pool, container, CHUNK_BIOMES_PER_SECTION, 0, out, &palette);
    CHECK(cr == CHUNK_OK, "biome decode succeeds");

    int mismatches = 0;
    for (uint32_t i = 0; i < CHUNK_BIOMES_PER_SECTION; i++) if (out[i] != indices[i]) mismatches++;
    CHECK(mismatches == 0, "every decoded biome index exactly matches what was packed");

    nbt_pool_free(&pool);
    free(buf);
}

static void test_truncated_data_is_rejected(void) {
    const char *names[6] = {
        "minecraft:a", "minecraft:b", "minecraft:c", "minecraft:d", "minecraft:e", "minecraft:f",
    };
    uint32_t indices[CHUNK_BLOCKS_PER_SECTION] = {0};

    uint8_t *buf;
    /* A container built for fewer cells than requested: its data array is
     * too short for a full section. */
    size_t len = chunk_synth_build_container_doc("block_states", names, 6, 1, 4,
                                                  16 /* far fewer than CHUNK_BLOCKS_PER_SECTION */,
                                                  indices, &buf);

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    CHECK(nbt_parse(&pool, buf, len, &root) == NBT_OK, "undersized container doc parses");

    nbt_tag_t *container = nbt_compound_getz(&pool, root, "block_states");
    uint16_t out[CHUNK_BLOCKS_PER_SECTION];
    const nbt_tag_t *palette;
    chunk_result_t cr = chunk_decode_paletted_container(&pool, container, CHUNK_BLOCKS_PER_SECTION, 4, out, &palette);
    CHECK(cr == CHUNK_ERR_DATA_TOO_SHORT, "decoding a full section from an undersized data array is rejected");

    nbt_pool_free(&pool);
    free(buf);
}

static void test_full_synthetic_chunk(void) {
    const int num_sections = 5;
    const uint32_t block_palette_size = 12;

    uint8_t *buf;
    size_t len = chunk_synth_build_chunk(0xC0FFEE, 4, -7, num_sections, block_palette_size, &buf);
    CHECK(len > 0, "synthetic chunk serializes to a non-empty buffer");

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    CHECK(nbt_parse(&pool, buf, len, &root) == NBT_OK, "synthetic chunk parses");

    nbt_tag_t *sections = nbt_compound_getz(&pool, root, "sections");
    CHECK(sections != NULL && sections->child_count == (uint32_t)num_sections,
          "chunk has the expected number of sections");

    for (uint32_t s = 0; sections && s < sections->child_count; s++) {
        nbt_tag_t *section = nbt_child_at(&pool, sections, s);

        uint16_t block_out[CHUNK_BLOCKS_PER_SECTION];
        const nbt_tag_t *block_palette;
        chunk_result_t bcr = chunk_decode_block_states(&pool, section, block_out, &block_palette);
        CHECK(bcr == CHUNK_OK, "block_states decode succeeds for every section");
        if (bcr == CHUNK_OK) {
            for (uint32_t i = 0; i < CHUNK_BLOCKS_PER_SECTION; i++) {
                if (block_out[i] >= block_palette->child_count) {
                    failures++;
                    fprintf(stderr, "FAIL: block index %u out of palette range at cell %u\n", block_out[i], i);
                    break;
                }
            }
        }

        uint16_t biome_out[CHUNK_BIOMES_PER_SECTION];
        const nbt_tag_t *biome_palette;
        chunk_result_t bicr = chunk_decode_biomes(&pool, section, biome_out, &biome_palette);
        CHECK(bicr == CHUNK_OK, "biomes decode succeeds for every section");
        if (bicr == CHUNK_OK) {
            CHECK(biome_palette->child_count == 4, "biome palette has the 4 fake entries the generator writes");
        }
    }

    nbt_pool_free(&pool);
    free(buf);
}

int main(void) {
    test_single_entry_palette();
    test_exact_round_trip_block_states();
    test_exact_round_trip_biomes();
    test_truncated_data_is_rejected();
    test_full_synthetic_chunk();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
