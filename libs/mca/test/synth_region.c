#include "synth_region.h"
#include "synth.h" /* nbt_synth_build_chunk, from ../nbt/test */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#define SECTOR_SIZE 4096u

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int mca_synth_write_region(const char *path, uint32_t seed, int num_chunks) {
    if (num_chunks < 0 || num_chunks > 1024) return -1;

    uint8_t *comp_data[1024] = {0};
    uLong comp_len[1024] = {0};
    uint32_t sector_offset[1024] = {0};
    uint8_t sector_count[1024] = {0};
    int ok = 1;

    uint32_t next_sector = 2; /* sectors 0,1 are the location/timestamp tables */

    for (int i = 0; i < num_chunks && ok; i++) {
        int32_t x = i % 32;
        int32_t z = i / 32;

        uint8_t *raw;
        size_t raw_len = nbt_synth_build_chunk(seed + (uint32_t)i, x, z, &raw);

        uLong bound = compressBound((uLong)raw_len);
        uint8_t *dest = (uint8_t *)malloc(bound);
        if (!dest) { free(raw); ok = 0; break; }

        uLong dest_len = bound;
        int zr = compress2(dest, &dest_len, raw, (uLong)raw_len, Z_DEFAULT_COMPRESSION);
        free(raw);
        if (zr != Z_OK) { free(dest); ok = 0; break; }

        comp_data[i] = dest;
        comp_len[i] = dest_len;

        size_t payload_len = 1 + dest_len;   /* scheme byte + compressed data */
        size_t record_size = 4 + payload_len; /* + the length field itself */
        uint32_t sectors = (uint32_t)((record_size + SECTOR_SIZE - 1) / SECTOR_SIZE);

        sector_offset[i] = next_sector;
        sector_count[i] = (uint8_t)sectors; /* fits: real chunks never need 255+ sectors */
        next_sector += sectors;
    }

    if (!ok) {
        for (int i = 0; i < num_chunks; i++) free(comp_data[i]);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        for (int i = 0; i < num_chunks; i++) free(comp_data[i]);
        return -1;
    }

    uint8_t locations[SECTOR_SIZE] = {0};
    uint8_t timestamps[SECTOR_SIZE] = {0};
    for (int i = 0; i < num_chunks; i++) {
        uint32_t word = (sector_offset[i] << 8) | sector_count[i];
        put_u32be(locations + i * 4, word);
        put_u32be(timestamps + i * 4, 1700000000u + (uint32_t)i);
    }

    int io_ok = 1;
    io_ok &= fwrite(locations, 1, SECTOR_SIZE, f) == SECTOR_SIZE;
    io_ok &= fwrite(timestamps, 1, SECTOR_SIZE, f) == SECTOR_SIZE;

    static const uint8_t zero_sector[SECTOR_SIZE] = {0};
    for (int i = 0; i < num_chunks && io_ok; i++) {
        size_t payload_len = 1 + comp_len[i];
        uint8_t hdr[5];
        put_u32be(hdr, (uint32_t)payload_len);
        hdr[4] = 2; /* zlib */

        io_ok &= fwrite(hdr, 1, 5, f) == 5;
        io_ok &= fwrite(comp_data[i], 1, comp_len[i], f) == comp_len[i];

        size_t written = 5 + comp_len[i];
        size_t sector_bytes = (size_t)sector_count[i] * SECTOR_SIZE;
        size_t pad = sector_bytes - written;
        while (pad > 0 && io_ok) {
            size_t chunk = pad > SECTOR_SIZE ? SECTOR_SIZE : pad;
            io_ok &= fwrite(zero_sector, 1, chunk, f) == chunk;
            pad -= chunk;
        }
    }

    fclose(f);
    for (int i = 0; i < num_chunks; i++) free(comp_data[i]);
    return io_ok ? 0 : -1;
}
