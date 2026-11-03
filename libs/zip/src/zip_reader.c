#define _POSIX_C_SOURCE 200809L

#include "zip_reader.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

/* ZIP multi-byte fields are little-endian (unlike NBT/PNG, both big-endian). */
static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define EOCD_SIG 0x06054b50u
#define CDFH_SIG 0x02014b50u
#define LFH_SIG  0x04034b50u
#define EOCD_MIN_SIZE 22
#define MAX_COMMENT_LEN 65535

static const uint8_t *find_eocd(const uint8_t *map, size_t map_len) {
    if (map_len < EOCD_MIN_SIZE) return NULL;
    size_t search_start = (map_len > (size_t)EOCD_MIN_SIZE + MAX_COMMENT_LEN)
        ? map_len - EOCD_MIN_SIZE - MAX_COMMENT_LEN : 0;
    /* scan backward: the signature is far more likely to be near the end
     * (comment is usually empty for jars) than the start. */
    for (size_t pos = map_len - EOCD_MIN_SIZE + 1; pos-- > search_start;) {
        if (rd_u32le(map + pos) == EOCD_SIG) return map + pos;
    }
    return NULL;
}

zip_result_t zip_open(const char *path, zip_archive_t *archive) {
    memset(archive, 0, sizeof(*archive));
    archive->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return ZIP_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return ZIP_ERR_IO; }
    size_t len = (size_t)st.st_size;

    void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return ZIP_ERR_IO; }

    archive->fd = fd;
    archive->map = (uint8_t *)map;
    archive->map_len = len;

    const uint8_t *eocd = find_eocd(archive->map, archive->map_len);
    if (!eocd) { zip_close(archive); return ZIP_ERR_NO_EOCD; }

    uint16_t total_entries = rd_u16le(eocd + 10);
    uint32_t cd_offset = rd_u32le(eocd + 16);
    if (cd_offset >= archive->map_len) { zip_close(archive); return ZIP_ERR_BAD_CENTRAL_DIR; }

    archive->entries = (zip_entry_t *)malloc(total_entries * sizeof(zip_entry_t));
    if (!archive->entries) { zip_close(archive); return ZIP_ERR_OOM; }

    const uint8_t *p = archive->map + cd_offset;
    const uint8_t *end = archive->map + archive->map_len;
    uint32_t found = 0;

    while (found < total_entries) {
        if (p + 46 > end || rd_u32le(p) != CDFH_SIG) break;

        uint16_t method = rd_u16le(p + 10);
        uint32_t compressed_size = rd_u32le(p + 20);
        uint32_t uncompressed_size = rd_u32le(p + 24);
        uint16_t name_len = rd_u16le(p + 28);
        uint16_t extra_len = rd_u16le(p + 30);
        uint16_t comment_len = rd_u16le(p + 32);
        uint32_t local_header_offset = rd_u32le(p + 42);
        const uint8_t *name = p + 46;

        if (name + name_len > end) break;

        zip_entry_t *e = &archive->entries[found++];
        e->name = (const char *)name;
        e->name_len = name_len;
        e->method = method;
        e->compressed_size = compressed_size;
        e->uncompressed_size = uncompressed_size;
        e->local_header_offset = local_header_offset;

        p = name + name_len + extra_len + comment_len;
    }

    archive->entry_count = found;
    return ZIP_OK;
}

void zip_close(zip_archive_t *archive) {
    if (!archive) return;
    free(archive->entries);
    if (archive->map) munmap(archive->map, archive->map_len);
    if (archive->fd >= 0) close(archive->fd);
    memset(archive, 0, sizeof(*archive));
}

const zip_entry_t *zip_find(const zip_archive_t *archive, const char *path, size_t path_len) {
    for (uint32_t i = 0; i < archive->entry_count; i++) {
        const zip_entry_t *e = &archive->entries[i];
        if (e->name_len == path_len && memcmp(e->name, path, path_len) == 0) return e;
    }
    return NULL;
}

zip_result_t zip_extract(const zip_archive_t *archive, const zip_entry_t *entry,
                         uint8_t **out_buf, size_t *out_len) {
    if (entry->method != 0 && entry->method != 8) return ZIP_ERR_UNSUPPORTED_METHOD;

    const uint8_t *lfh = archive->map + entry->local_header_offset;
    if (lfh + 30 > archive->map + archive->map_len || rd_u32le(lfh) != LFH_SIG) {
        return ZIP_ERR_BAD_CENTRAL_DIR;
    }
    uint16_t name_len = rd_u16le(lfh + 26);
    uint16_t extra_len = rd_u16le(lfh + 28);
    const uint8_t *data = lfh + 30 + name_len + extra_len;
    if (data + entry->compressed_size > archive->map + archive->map_len) return ZIP_ERR_BAD_CENTRAL_DIR;

    if (entry->method == 0) {
        uint8_t *buf = (uint8_t *)malloc(entry->uncompressed_size ? entry->uncompressed_size : 1);
        if (!buf) return ZIP_ERR_OOM;
        memcpy(buf, data, entry->uncompressed_size);
        *out_buf = buf;
        *out_len = entry->uncompressed_size;
        return ZIP_OK;
    }

    /* method == 8: raw DEFLATE, no zlib/gzip wrapper - negative windowBits
     * tells zlib to skip expecting one. */
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -15) != Z_OK) return ZIP_ERR_INFLATE;

    uint8_t *buf = (uint8_t *)malloc(entry->uncompressed_size ? entry->uncompressed_size : 1);
    if (!buf) { inflateEnd(&strm); return ZIP_ERR_OOM; }

    strm.next_in = (Bytef *)data;
    strm.avail_in = entry->compressed_size;
    strm.next_out = buf;
    strm.avail_out = entry->uncompressed_size;

    int zr = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (zr != Z_STREAM_END) { free(buf); return ZIP_ERR_INFLATE; }

    *out_buf = buf;
    *out_len = entry->uncompressed_size;
    return ZIP_OK;
}
