/*
 * mca.h - reader for Anvil region files (.mca); format per
 * minecraft.wiki "Region file format", summarized in README.md.
 *
 * A region covers 32x32 chunks:
 *
 *   bytes         0..4095   : location table, 1024 x 4 bytes (3-byte
 *                             big-endian sector offset, 1-byte sector
 *                             count), indexed by (x&31)+(z&31)*32
 *   bytes      4096..8191   : timestamp table, 1024 x 4 bytes (big-endian
 *                             epoch seconds), same index
 *   bytes 8192..            : chunk payloads at sector_offset*4096: 4-byte
 *                             big-endian length, 1-byte compression, then
 *                             (length-1) bytes of compressed NBT
 *
 * The file is mmap'd read-only; only the decompressed buffer is written.
 */
#ifndef PRISM_MCA_H
#define PRISM_MCA_H

#include "nbt.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mca_result {
    MCA_OK = 0,
    MCA_ERR_IO,              /* open/fstat/mmap failed */
    MCA_ERR_TOO_SMALL,       /* file shorter than the 8KiB header */
    MCA_ERR_BOUNDS,          /* local coords or a header entry point outside the file */
    MCA_ERR_EMPTY,           /* chunk slot has no data (never generated) */
    MCA_ERR_EXTERNAL,        /* chunk payload lives in a sibling .mcc file (unsupported) */
    MCA_ERR_BAD_COMPRESSION, /* unknown/unsupported compression scheme byte */
    MCA_ERR_INFLATE,         /* zlib/gzip decompression failed */
    MCA_ERR_NBT,             /* decompressed fine but NBT parsing failed */
} mca_result_t;

typedef enum mca_compression {
    MCA_COMPRESSION_GZIP = 1,
    MCA_COMPRESSION_ZLIB = 2,
    MCA_COMPRESSION_NONE = 3,
    MCA_COMPRESSION_LZ4  = 4, /* not implemented, see README */
} mca_compression_t;

typedef struct mca_region {
    int      fd;
    uint8_t *map;     /* mmap base, MAP_PRIVATE|PROT_READ */
    size_t   map_len;
} mca_region_t;

typedef struct mca_chunk_loc {
    uint32_t sector_offset; /* in 4096-byte sectors from file start */
    uint8_t  sector_count;
    uint32_t timestamp;     /* epoch seconds, from the timestamp table */
} mca_chunk_loc_t;

/* mmaps `path` read-only. Returns MCA_OK on success; region is safe to
 * pass to mca_region_close() either way. */
mca_result_t mca_region_open(const char *path, mca_region_t *region);
void mca_region_close(mca_region_t *region);

/* local_x/local_z in [0, 32). An all-zero loc means the chunk was never
 * generated (mca_chunk_present). */
mca_result_t mca_region_get_loc(const mca_region_t *region, int local_x, int local_z, mca_chunk_loc_t *out);
static inline int mca_chunk_present(const mca_chunk_loc_t *loc) {
    return loc->sector_offset != 0 || loc->sector_count != 0;
}

/* Decompresses and parses chunk (local_x, local_z). On MCA_OK the pool
 * owns the buffer; one nbt_pool_free() frees both. */
mca_result_t mca_region_parse_chunk(const mca_region_t *region, int local_x, int local_z,
                                     nbt_pool_t *pool, nbt_tag_t **out_root);

/* Visits every present chunk, z outer, x inner. `pool`/`root` are valid
 * only during the callback. A non-zero return stops the walk (reported in
 * *out_stopped_early if non-NULL). Chunks that fail to parse are skipped
 * and counted in *out_failed if non-NULL. */
mca_result_t mca_region_for_each_chunk(
    const mca_region_t *region,
    int (*cb)(int local_x, int local_z, nbt_pool_t *pool, nbt_tag_t *root, void *userdata),
    void *userdata,
    int *out_failed);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_MCA_H */
