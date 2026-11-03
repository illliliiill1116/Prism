#include "nbt_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static nbt_result_t read_whole_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NBT_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NBT_ERR_IO; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NBT_ERR_IO; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NBT_ERR_IO; }

    uint8_t *buf = (uint8_t *)malloc((size_t)size ? (size_t)size : 1);
    if (!buf) { fclose(f); return NBT_ERR_OOM; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NBT_ERR_IO; }

    *out_buf = buf;
    *out_len = (size_t)size;
    return NBT_OK;
}

/* windowBits = 15 | 32 makes zlib auto-detect either a zlib or gzip
 * header, so callers don't need to know which wrapper was used. */
nbt_result_t nbt_io_inflate(const uint8_t *in, size_t in_len, uint8_t **out_buf, size_t *out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 15 | 32) != Z_OK) return NBT_ERR_INFLATE;

    size_t cap = in_len * 4 > 4096 ? in_len * 4 : 4096;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) { inflateEnd(&strm); return NBT_ERR_OOM; }

    strm.next_in = (Bytef *)in;
    strm.avail_in = (uInt)in_len;

    size_t total = 0;
    int zret;
    do {
        if (total == cap) {
            size_t new_cap = cap * 2;
            uint8_t *grown = (uint8_t *)realloc(out, new_cap);
            if (!grown) { free(out); inflateEnd(&strm); return NBT_ERR_OOM; }
            out = grown;
            cap = new_cap;
        }
        strm.next_out = out + total;
        strm.avail_out = (uInt)(cap - total);
        zret = inflate(&strm, Z_NO_FLUSH);
        if (zret != Z_OK && zret != Z_STREAM_END) {
            free(out);
            inflateEnd(&strm);
            return NBT_ERR_INFLATE;
        }
        total = cap - strm.avail_out;
    } while (zret != Z_STREAM_END);

    inflateEnd(&strm);
    *out_buf = out;
    *out_len = total;
    return NBT_OK;
}

nbt_result_t nbt_io_load_file(const char *path, nbt_pool_t *pool, nbt_tag_t **out_root) {
    uint8_t *raw;
    size_t raw_len;
    nbt_result_t r = read_whole_file(path, &raw, &raw_len);
    if (r != NBT_OK) return r;

    uint8_t *data;
    size_t data_len;
    /* gzip: 1F 8B. zlib: 0x78 in practice. Anything else is raw NBT. */
    int compressed = raw_len >= 2 && (
        (raw[0] == 0x1F && raw[1] == 0x8B) ||
        (raw[0] == 0x78)
    );

    if (compressed) {
        r = nbt_io_inflate(raw, raw_len, &data, &data_len);
        free(raw);
        if (r != NBT_OK) return r;
    } else {
        data = raw;
        data_len = raw_len;
    }

    nbt_pool_init(pool);
    pool->data = data;
    pool->data_len = data_len;
    pool->owns_data = 1;

    r = nbt_parse(pool, data, data_len, out_root);
    if (r != NBT_OK) {
        nbt_pool_free(pool); /* also frees `data` */
    }
    return r;
}
