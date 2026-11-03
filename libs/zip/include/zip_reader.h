/*
 * zip_reader.h - a minimal read-only ZIP/JAR reader: open an archive, look
 * up an entry by exact path, extract it to memory. No ZIP64, multi-disk
 * archives, encryption or writing (README.md).
 */
#ifndef PRISM_ZIP_READER_H
#define PRISM_ZIP_READER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zip_result {
    ZIP_OK = 0,
    ZIP_ERR_IO,
    ZIP_ERR_NO_EOCD,       /* not a ZIP file, or a form this reader doesn't understand (e.g. ZIP64) */
    ZIP_ERR_BAD_CENTRAL_DIR,
    ZIP_ERR_NOT_FOUND,     /* zip_find(): no entry with that exact path */
    ZIP_ERR_UNSUPPORTED_METHOD, /* compression method other than 0 (stored) or 8 (deflate) */
    ZIP_ERR_INFLATE,
    ZIP_ERR_OOM,
} zip_result_t;

typedef struct zip_entry {
    const char *name;   /* points into the archive's mmap, NOT null-terminated */
    uint16_t name_len;
    uint16_t method;    /* 0 = stored, 8 = deflate */
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
} zip_entry_t;

typedef struct zip_archive {
    int fd;
    uint8_t *map;
    size_t map_len;

    zip_entry_t *entries;
    uint32_t entry_count;
} zip_archive_t;

/* mmaps `path` and parses its central directory (does not read/inflate
 * any entry data yet - that happens per-entry in zip_extract). */
zip_result_t zip_open(const char *path, zip_archive_t *archive);
void zip_close(zip_archive_t *archive);

/* Linear search; a jar has a few thousand entries, and lookups happen once
 * per distinct block, model or texture. */
const zip_entry_t *zip_find(const zip_archive_t *archive, const char *path, size_t path_len);
static inline const zip_entry_t *zip_findz(const zip_archive_t *archive, const char *path) {
    size_t len = 0;
    while (path[len]) len++;
    return zip_find(archive, path, len);
}

/* Extracts `entry` into a freshly malloc'd buffer. Caller owns *out_buf. */
zip_result_t zip_extract(const zip_archive_t *archive, const zip_entry_t *entry,
                         uint8_t **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_ZIP_READER_H */
