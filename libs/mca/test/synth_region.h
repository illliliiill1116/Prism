/* Synthetic .mca region file generator for the tests. Reuses
 * ../nbt/test/synth.c's per-chunk NBT builder and zlib-compresses each
 * chunk the way the game does. */
#ifndef PRISM_MCA_SYNTH_REGION_H
#define PRISM_MCA_SYNTH_REGION_H

#include <stdint.h>

/* Writes a region to `path` with `num_chunks` (<= 1024) present chunks,
 * row-major from (0,0). 0 on success, -1 on failure. Deterministic in
 * `seed`. */
int mca_synth_write_region(const char *path, uint32_t seed, int num_chunks);

#endif
