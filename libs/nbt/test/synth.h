/* Synthetic Anvil-chunk-shaped NBT for the tests (not part of libnbt). */
#ifndef PRISM_NBT_SYNTH_H
#define PRISM_NBT_SYNTH_H

#include <stddef.h>
#include <stdint.h>

/* A big-endian NBT buffer like a batch of chunks: a root compound with
 * `num_chunks` chunks of 24 sections each. Deterministic in `seed`.
 * Caller frees *out_buf; returns its length. */
size_t nbt_synth_build_world(uint32_t seed, int num_chunks, uint8_t **out_buf);

/* One standalone chunk document (as inside nbt_synth_build_world), the
 * payload of a region chunk; ../mca's tests use it. Caller frees
 * *out_buf. */
size_t nbt_synth_build_chunk(uint32_t seed, int32_t x, int32_t z, uint8_t **out_buf);

#endif
