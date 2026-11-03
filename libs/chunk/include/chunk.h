/*
 * chunk.h - decodes the paletted containers in a 1.18+ chunk section
 * (DataVersion >= 2529): block_states and biomes, from the nbt_tag_t tree
 * a chunk parses into, to a flat per-cell palette-index array. Format:
 * minecraft.wiki "Chunk format", summarized in README.md.
 *
 * Mapping a palette name to anything (ids, colours, models) is left to the
 * layers above.
 */
#ifndef PRISM_CHUNK_H
#define PRISM_CHUNK_H

#include "nbt.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A section (16x16x16 blocks) has 4096 block-state cells and 64 biome
 * cells (biomes are stored at 4x4x4 granularity). */
#define CHUNK_BLOCKS_PER_SECTION 4096u
#define CHUNK_BIOMES_PER_SECTION 64u

typedef enum chunk_result {
    CHUNK_OK = 0,
    CHUNK_ERR_MISSING_TAG,     /* section has no block_states/biomes, or that compound has no "palette" */
    CHUNK_ERR_BAD_TAG_TYPE,    /* a tag was present but the wrong NBT type */
    CHUNK_ERR_EMPTY_PALETTE,   /* palette list has zero entries (always invalid) */
    CHUNK_ERR_DATA_TOO_SHORT,  /* "data" long array is shorter than bits_per_entry implies */
} chunk_result_t;

/*
 * Decodes a paletted container (a section's "block_states" or "biomes"
 * compound) into out_indices[expected_count], each an index into the
 * palette.
 *
 * min_bits_per_entry is 4 for block_states and 0 for biomes (README.md);
 * prefer the chunk_decode_block_states/chunk_decode_biomes wrappers.
 *
 * *out_palette points at the container's "palette" list (not copied),
 * valid as long as the nbt pool.
 */
chunk_result_t chunk_decode_paletted_container(
    const nbt_pool_t *pool,
    const nbt_tag_t *container,
    uint32_t expected_count,
    uint32_t min_bits_per_entry,
    uint16_t *out_indices,
    const nbt_tag_t **out_palette);

static inline chunk_result_t chunk_decode_block_states(
    const nbt_pool_t *pool, const nbt_tag_t *section,
    uint16_t out_indices[/* CHUNK_BLOCKS_PER_SECTION */], const nbt_tag_t **out_palette) {
    const nbt_tag_t *bs = nbt_compound_getz(pool, section, "block_states");
    if (!bs) return CHUNK_ERR_MISSING_TAG;
    return chunk_decode_paletted_container(pool, bs, CHUNK_BLOCKS_PER_SECTION, 4, out_indices, out_palette);
}

static inline chunk_result_t chunk_decode_biomes(
    const nbt_pool_t *pool, const nbt_tag_t *section,
    uint16_t out_indices[/* CHUNK_BIOMES_PER_SECTION */], const nbt_tag_t **out_palette) {
    const nbt_tag_t *bi = nbt_compound_getz(pool, section, "biomes");
    if (!bi) return CHUNK_ERR_MISSING_TAG;
    return chunk_decode_paletted_container(pool, bi, CHUNK_BIOMES_PER_SECTION, 0, out_indices, out_palette);
}

/*
 * The name of palette entry `index`, for both entry shapes:
 *   - biomes:       a plain string, e.g. "minecraft:plains"
 *   - block_states: a compound with "Name" and optional "Properties"
 */
chunk_result_t chunk_palette_entry_name(
    const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t index,
    const char **out_name, uint16_t *out_name_len);

/*
 * One blockstate property of palette entry `index` ("facing" -> "east").
 * CHUNK_ERR_MISSING_TAG when the entry has no "Properties" or not this key
 * - both normal. The value is not null-terminated; compare by length.
 */
chunk_result_t chunk_palette_entry_property(
    const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t index,
    const char *key, const char **out_value, uint16_t *out_value_len);

/*
 * Sets *out_properties to palette entry `index`'s raw "Properties"
 * compound (not copied, valid as long as `pool`), or NULL when it has none. For callers that enumerate
 * every property: iterate with nbt_child_at(); each child's name is the
 * property and its string the value.
 */
chunk_result_t chunk_palette_entry_properties(
    const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t index,
    const nbt_tag_t **out_properties);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_CHUNK_H */
