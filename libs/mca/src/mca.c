#define _POSIX_C_SOURCE 200809L

#include "mca.h"
#include "nbt_io.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR_SIZE 4096u
#define HEADER_SIZE (2u * SECTOR_SIZE)

static inline uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

mca_result_t mca_region_open(const char *path, mca_region_t *region) {
    memset(region, 0, sizeof(*region));
    region->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return MCA_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return MCA_ERR_IO; }
    size_t len = (size_t)st.st_size;
    if (len < HEADER_SIZE) { close(fd); return MCA_ERR_TOO_SMALL; }

    void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return MCA_ERR_IO; }

    region->fd = fd;
    region->map = (uint8_t *)map;
    region->map_len = len;
    return MCA_OK;
}

void mca_region_close(mca_region_t *region) {
    if (!region) return;
    if (region->map) munmap(region->map, region->map_len);
    if (region->fd >= 0) close(region->fd);
    memset(region, 0, sizeof(*region));
}

mca_result_t mca_region_get_loc(const mca_region_t *region, int local_x, int local_z, mca_chunk_loc_t *out) {
    if (local_x < 0 || local_x >= 32 || local_z < 0 || local_z >= 32) return MCA_ERR_BOUNDS;

    uint32_t index = (uint32_t)local_x + (uint32_t)local_z * 32u;
    const uint8_t *loc_entry = region->map + index * 4u;
    const uint8_t *ts_entry = region->map + SECTOR_SIZE + index * 4u;

    uint32_t loc_word = rd_u32be(loc_entry);
    out->sector_offset = loc_word >> 8;
    out->sector_count = (uint8_t)(loc_word & 0xFF);
    out->timestamp = rd_u32be(ts_entry);
    return MCA_OK;
}

mca_result_t mca_region_parse_chunk(const mca_region_t *region, int local_x, int local_z,
                                     nbt_pool_t *pool, nbt_tag_t **out_root) {
    mca_chunk_loc_t loc;
    mca_result_t r = mca_region_get_loc(region, local_x, local_z, &loc);
    if (r != MCA_OK) return r;
    if (!mca_chunk_present(&loc)) return MCA_ERR_EMPTY;

    size_t byte_offset = (size_t)loc.sector_offset * SECTOR_SIZE;
    size_t sector_bytes = (size_t)loc.sector_count * SECTOR_SIZE;
    if (byte_offset + 5 > region->map_len) return MCA_ERR_BOUNDS;
    if (byte_offset + sector_bytes > region->map_len) return MCA_ERR_BOUNDS;

    uint32_t payload_len = rd_u32be(region->map + byte_offset);
    if (payload_len < 1 || byte_offset + 4 + payload_len > region->map_len) return MCA_ERR_BOUNDS;

    uint8_t scheme = region->map[byte_offset + 4];
    int external = scheme & 0x80;
    scheme &= 0x7F;

    if (external) return MCA_ERR_EXTERNAL; /* payload lives in r.X.Z.c.N.mcc, not handled here */

    const uint8_t *payload = region->map + byte_offset + 5;
    size_t payload_size = payload_len - 1;

    nbt_pool_init(pool);

    switch (scheme) {
    case MCA_COMPRESSION_GZIP:
    case MCA_COMPRESSION_ZLIB: {
        uint8_t *decompressed;
        size_t decompressed_len;
        nbt_result_t ir = nbt_io_inflate(payload, payload_size, &decompressed, &decompressed_len);
        if (ir != NBT_OK) return MCA_ERR_INFLATE;
        pool->data = decompressed;
        pool->data_len = decompressed_len;
        pool->owns_data = 1;
        nbt_result_t pr = nbt_parse(pool, decompressed, decompressed_len, out_root);
        if (pr != NBT_OK) { nbt_pool_free(pool); return MCA_ERR_NBT; }
        return MCA_OK;
    }
    case MCA_COMPRESSION_NONE: {
        /* the mmap is read-only and nbt_parse byte-swaps arrays in place,
         * so this must be copied into a writable buffer first. */
        uint8_t *copy = (uint8_t *)malloc(payload_size);
        if (!copy) return MCA_ERR_IO;
        memcpy(copy, payload, payload_size);
        pool->data = copy;
        pool->data_len = payload_size;
        pool->owns_data = 1;
        nbt_result_t pr = nbt_parse(pool, copy, payload_size, out_root);
        if (pr != NBT_OK) { nbt_pool_free(pool); return MCA_ERR_NBT; }
        return MCA_OK;
    }
    default:
        return MCA_ERR_BAD_COMPRESSION;
    }
}

mca_result_t mca_region_for_each_chunk(
    const mca_region_t *region,
    int (*cb)(int local_x, int local_z, nbt_pool_t *pool, nbt_tag_t *root, void *userdata),
    void *userdata,
    int *out_failed) {

    int failed = 0;
    for (int z = 0; z < 32; z++) {
        for (int x = 0; x < 32; x++) {
            mca_chunk_loc_t loc;
            mca_region_get_loc(region, x, z, &loc);
            if (!mca_chunk_present(&loc)) continue;

            nbt_pool_t pool;
            nbt_tag_t *root;
            mca_result_t r = mca_region_parse_chunk(region, x, z, &pool, &root);
            if (r != MCA_OK) { failed++; continue; }

            int stop = cb(x, z, &pool, root, userdata);
            nbt_pool_free(&pool);
            if (stop) {
                if (out_failed) *out_failed = failed;
                return MCA_OK;
            }
        }
    }
    if (out_failed) *out_failed = failed;
    return MCA_OK;
}
