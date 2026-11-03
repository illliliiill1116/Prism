/*
 * nbt_io.h - loads NBT from disk, handling level.dat's gzip and region
 * chunks' zlib. The pool takes ownership of the decompressed buffer, so
 * one nbt_pool_free() frees everything.
 */
#ifndef PRISM_NBT_IO_H
#define PRISM_NBT_IO_H

#include "nbt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads `path`, decompresses it if it is gzip or zlib (by magic bytes) and
 * parses it. On failure the pool is empty and safe to free. */
nbt_result_t nbt_io_load_file(const char *path, nbt_pool_t *pool, nbt_tag_t **out_root);

/* Inflates a zlib or gzip buffer into a new malloc'd one. The caller owns
 * *out_buf (or hands it to a pool via pool->data/data_len/owns_data). */
nbt_result_t nbt_io_inflate(const uint8_t *in, size_t in_len, uint8_t **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_NBT_IO_H */
