/*
 * nbt.h - a zero-copy, allocation-light NBT parser (format summarized in
 * README.md).
 *
 *  - Tags live in one flat growable array (nbt_pool_t.tags); compound and
 *    list children are index ranges into a second array, since indices
 *    survive realloc and pointers don't. nbt_child_at() resolves one.
 *  - Strings and byte arrays point into the input buffer; int/long arrays
 *    are byte-swapped in place there. The buffer must outlive the pool.
 *  - After nbt_parse() the pool is read-only; tag pointers stay valid
 *    until nbt_pool_free().
 */
#ifndef PRISM_NBT_H
#define PRISM_NBT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nbt_type {
    NBT_END        = 0,
    NBT_BYTE       = 1,
    NBT_SHORT      = 2,
    NBT_INT        = 3,
    NBT_LONG       = 4,
    NBT_FLOAT      = 5,
    NBT_DOUBLE     = 6,
    NBT_BYTE_ARRAY = 7,
    NBT_STRING     = 8,
    NBT_LIST       = 9,
    NBT_COMPOUND   = 10,
    NBT_INT_ARRAY  = 11,
    NBT_LONG_ARRAY = 12,
} nbt_type_t;

typedef enum nbt_result {
    NBT_OK = 0,
    NBT_ERR_TRUNCATED,      /* buffer ended mid-tag */
    NBT_ERR_BAD_TAG,        /* tag id outside 0..12 */
    NBT_ERR_TOO_DEEP,       /* nesting exceeds NBT_MAX_DEPTH, likely corrupt */
    NBT_ERR_OOM,            /* malloc/realloc failed */
    NBT_ERR_NEGATIVE_LEN,   /* array/list/string length < 0 */
    NBT_ERR_IO,             /* file open/read failed (nbt_io) */
    NBT_ERR_INFLATE,        /* zlib/gzip decompression failed (nbt_io) */
} nbt_result_t;

#ifndef NBT_MAX_DEPTH
#define NBT_MAX_DEPTH 512
#endif

typedef struct nbt_tag {
    uint8_t  type;             /* nbt_type_t */
    uint8_t  list_elem_type;   /* meaningful only when type == NBT_LIST */
    uint16_t name_len;
    const char *name;          /* zero-copy, NOT null-terminated, NULL for unnamed (list elements) */

    /* for NBT_COMPOUND / NBT_LIST: index range into pool->children[] */
    uint32_t child_start;
    uint32_t child_count;

    union {
        int8_t   v_byte;
        int16_t  v_short;
        int32_t  v_int;
        int64_t  v_long;
        float    v_float;
        double   v_double;
        struct { int32_t len; const int8_t  *data; } byte_array;
        struct { int32_t len; const char    *data; } string; /* zero-copy, not null-terminated */
        struct { int32_t len; const int32_t *data; } int_array;
        struct { int32_t len; const int64_t *data; } long_array;
    };
} nbt_tag_t;

typedef struct nbt_pool {
    nbt_tag_t *tags;
    uint32_t   tag_count;
    uint32_t   tag_cap;

    uint32_t  *children;
    uint32_t   children_count;
    uint32_t   children_cap;

    /* The buffer tags point into. Freed by nbt_pool_free only if
     * owns_data is set (how nbt_io hands over a decompressed buffer). */
    uint8_t   *data;
    size_t     data_len;
    int        owns_data;
} nbt_pool_t;

void nbt_pool_init(nbt_pool_t *pool);
void nbt_pool_free(nbt_pool_t *pool);

/*
 * Parses one big-endian NBT document from `data`, which is not copied:
 * payloads point into it and int/long arrays are swapped in place, so it
 * must stay alive and writable while the pool is used. Set
 * pool->data/data_len/owns_data first to have the pool free it, or use
 * nbt_io's loaders.
 */
nbt_result_t nbt_parse(nbt_pool_t *pool, uint8_t *data, size_t len, nbt_tag_t **out_root);

/* Resolve child #i (0-indexed) of a COMPOUND or LIST tag. No bounds check
 * against i >= child_count - callers should check parent->child_count. */
static inline nbt_tag_t *nbt_child_at(const nbt_pool_t *pool, const nbt_tag_t *parent, uint32_t i) {
    return &pool->tags[pool->children[parent->child_start + i]];
}

/* Linear search for a named child of a COMPOUND tag. Returns NULL if absent.
 * name need not be null-terminated NBT strings; pass its length explicitly. */
nbt_tag_t *nbt_compound_get(const nbt_pool_t *pool, const nbt_tag_t *compound, const char *name, size_t name_len);
/* Convenience wrapper for a null-terminated C string key. */
nbt_tag_t *nbt_compound_getz(const nbt_pool_t *pool, const nbt_tag_t *compound, const char *name);

const char *nbt_type_name(uint8_t type);

/* Counts every tag under and including `root`. */
uint64_t nbt_count_tags(const nbt_pool_t *pool, const nbt_tag_t *root);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_NBT_H */
