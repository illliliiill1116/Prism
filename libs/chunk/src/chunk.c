#include "chunk.h"

#include <string.h>

/* ceil(log2(n)) for n >= 1; bits_needed(1) == 0. */
static uint32_t bits_needed(uint32_t n) {
    uint32_t bits = 0;
    uint32_t v = 1;
    while (v < n) {
        v <<= 1;
        bits++;
    }
    return bits;
}

chunk_result_t chunk_decode_paletted_container(
    const nbt_pool_t *pool,
    const nbt_tag_t *container,
    uint32_t expected_count,
    uint32_t min_bits_per_entry,
    uint16_t *out_indices,
    const nbt_tag_t **out_palette) {

    const nbt_tag_t *palette = nbt_compound_getz(pool, container, "palette");
    if (!palette) return CHUNK_ERR_MISSING_TAG;
    if (palette->type != NBT_LIST) return CHUNK_ERR_BAD_TAG_TYPE;

    uint32_t palette_count = palette->child_count;
    if (palette_count == 0) return CHUNK_ERR_EMPTY_PALETTE;

    *out_palette = palette;

    /* Single-entry palette: every cell is implicitly index 0, and the
     * "data" long array is omitted from the NBT entirely. */
    if (palette_count == 1) {
        memset(out_indices, 0, expected_count * sizeof(uint16_t));
        return CHUNK_OK;
    }

    uint32_t bits = bits_needed(palette_count);
    if (bits < min_bits_per_entry) bits = min_bits_per_entry;

    const nbt_tag_t *data = nbt_compound_getz(pool, container, "data");
    if (!data) return CHUNK_ERR_MISSING_TAG;
    if (data->type != NBT_LONG_ARRAY) return CHUNK_ERR_BAD_TAG_TYPE;

    uint32_t values_per_long = 64u / bits;
    uint32_t num_longs_needed = (expected_count + values_per_long - 1) / values_per_long;
    if ((uint32_t)data->long_array.len < num_longs_needed) return CHUNK_ERR_DATA_TOO_SHORT;

    const int64_t *longs = data->long_array.data;
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);

    /* Since 1.16 entries never straddle a long, so each long unpacks
     * independently. */
    uint32_t idx = 0;
    for (uint32_t li = 0; li < num_longs_needed && idx < expected_count; li++) {
        uint64_t word = (uint64_t)longs[li];
        for (uint32_t j = 0; j < values_per_long && idx < expected_count; j++, idx++) {
            out_indices[idx] = (uint16_t)((word >> (j * bits)) & mask);
        }
    }

    return CHUNK_OK;
}

chunk_result_t chunk_palette_entry_name(
    const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t index,
    const char **out_name, uint16_t *out_name_len) {

    const nbt_tag_t *entry = nbt_child_at(pool, palette, index);

    if (entry->type == NBT_STRING) {
        *out_name = entry->string.data;
        *out_name_len = (uint16_t)entry->string.len;
        return CHUNK_OK;
    }

    if (entry->type == NBT_COMPOUND) {
        const nbt_tag_t *name_tag = nbt_compound_getz(pool, entry, "Name");
        if (!name_tag || name_tag->type != NBT_STRING) return CHUNK_ERR_BAD_TAG_TYPE;
        *out_name = name_tag->string.data;
        *out_name_len = (uint16_t)name_tag->string.len;
        return CHUNK_OK;
    }

    return CHUNK_ERR_BAD_TAG_TYPE;
}

chunk_result_t chunk_palette_entry_properties(
    const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t index,
    const nbt_tag_t **out_properties) {

    const nbt_tag_t *entry = nbt_child_at(pool, palette, index);
    if (entry->type != NBT_COMPOUND) { *out_properties = NULL; return CHUNK_OK; }

    const nbt_tag_t *props = nbt_compound_getz(pool, entry, "Properties");
    *out_properties = (props && props->type == NBT_COMPOUND) ? props : NULL;
    return CHUNK_OK;
}

chunk_result_t chunk_palette_entry_property(
    const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t index,
    const char *key, const char **out_value, uint16_t *out_value_len) {

    const nbt_tag_t *entry = nbt_child_at(pool, palette, index);
    if (entry->type != NBT_COMPOUND) return CHUNK_ERR_MISSING_TAG;

    const nbt_tag_t *props = nbt_compound_getz(pool, entry, "Properties");
    if (!props || props->type != NBT_COMPOUND) return CHUNK_ERR_MISSING_TAG;

    const nbt_tag_t *value_tag = nbt_compound_getz(pool, props, key);
    if (!value_tag || value_tag->type != NBT_STRING) return CHUNK_ERR_MISSING_TAG;

    *out_value = value_tag->string.data;
    *out_value_len = (uint16_t)value_tag->string.len;
    return CHUNK_OK;
}
