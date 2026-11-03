/* Synthetic chunk NBT shaped like real 1.18+ paletted sections (palette +
 * packed long array), for the tests. ../nbt/test/synth.c is a different
 * generator: flat arrays, no palette packing. */
#ifndef PRISM_CHUNK_SYNTH_SECTION_H
#define PRISM_CHUNK_SYNTH_SECTION_H

#include <stddef.h>
#include <stdint.h>

/* A chunk NBT document with `num_sections` sections, each with a packed
 * block_states container (`block_palette_size` fake names) and a biomes
 * container (4 fake names). Deterministic in `seed`. Caller frees
 * *out_buf. */
size_t chunk_synth_build_chunk(uint32_t seed, int32_t x, int32_t z,
                               int num_sections, uint32_t block_palette_size,
                               uint8_t **out_buf);

/* An NBT document with one paletted container under `container_key`,
 * packing exactly `indices[count]` (each < palette_size), so tests can
 * round-trip chosen values. `entry_is_compound` as above. Caller frees
 * *out_buf. */
size_t chunk_synth_build_container_doc(const char *container_key,
                                       const char *const *names, uint32_t palette_size,
                                       int entry_is_compound, uint32_t min_bits,
                                       uint32_t count, const uint32_t *indices,
                                       uint8_t **out_buf);

#endif
