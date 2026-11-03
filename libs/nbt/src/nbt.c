#include "nbt.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- */
/* pool: flat growable arrays, referenced by index (stable across       */
/* realloc), not by pointer.                                            */
/* ------------------------------------------------------------------- */

void nbt_pool_init(nbt_pool_t *pool) {
    memset(pool, 0, sizeof(*pool));
}

void nbt_pool_free(nbt_pool_t *pool) {
    if (!pool) return;
    free(pool->tags);
    free(pool->children);
    if (pool->owns_data) free(pool->data);
    memset(pool, 0, sizeof(*pool));
}

static int pool_reserve_tags(nbt_pool_t *pool, uint32_t need) {
    if (pool->tag_count + need <= pool->tag_cap) return 0;
    uint32_t cap = pool->tag_cap ? pool->tag_cap * 2 : 256;
    while (cap < pool->tag_count + need) cap *= 2;
    nbt_tag_t *grown = (nbt_tag_t *)realloc(pool->tags, cap * sizeof(nbt_tag_t));
    if (!grown) return -1;
    pool->tags = grown;
    pool->tag_cap = cap;
    return 0;
}

static int pool_reserve_children(nbt_pool_t *pool, uint32_t need) {
    if (pool->children_count + need <= pool->children_cap) return 0;
    uint32_t cap = pool->children_cap ? pool->children_cap * 2 : 256;
    while (cap < pool->children_count + need) cap *= 2;
    uint32_t *grown = (uint32_t *)realloc(pool->children, cap * sizeof(uint32_t));
    if (!grown) return -1;
    pool->children = grown;
    pool->children_cap = cap;
    return 0;
}

/* Allocates one tag slot and returns its index, or UINT32_MAX on OOM. */
static uint32_t pool_alloc_tag(nbt_pool_t *pool) {
    if (pool_reserve_tags(pool, 1) != 0) return UINT32_MAX;
    uint32_t idx = pool->tag_count++;
    memset(&pool->tags[idx], 0, sizeof(nbt_tag_t));
    return idx;
}

/* ------------------------------------------------------------------- */
/* big-endian reads / in-place byte swaps                               */
/* ------------------------------------------------------------------- */

static inline uint16_t rd_u16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t rd_u64be(const uint8_t *p) {
    return ((uint64_t)rd_u32be(p) << 32) | rd_u32be(p + 4);
}

/* Byte-swaps `count` big-endian int32_t's in place, starting at p. */
static void bswap32_inplace(uint8_t *p, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        uint8_t *e = p + (size_t)i * 4;
        uint32_t v = rd_u32be(e);
        memcpy(e, &v, 4); /* host is little-endian on every target we build for */
    }
}
static void bswap64_inplace(uint8_t *p, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        uint8_t *e = p + (size_t)i * 8;
        uint64_t v = rd_u64be(e);
        memcpy(e, &v, 8);
    }
}

/* ------------------------------------------------------------------- */
/* cursor: bounds-checked walk over the input buffer                    */
/* ------------------------------------------------------------------- */

typedef struct {
    uint8_t *base;
    size_t   pos;
    size_t   len;
    nbt_pool_t *pool;
} cursor_t;

#define NEED(cur, n) do { if ((cur)->pos + (size_t)(n) > (cur)->len) return NBT_ERR_TRUNCATED; } while (0)

/* ------------------------------------------------------------------- */
/* small-vector child-index buffer                                      */
/*                                                                       */
/* A compound/list's direct children must land in a *contiguous* run of */
/* pool->children[], but parsing a child recursively appends that       */
/* child's own descendants to the same flat array first. So each stack  */
/* frame accumulates its direct children locally (inline for the common */
/* case of <=32, spilling to the heap only for large lists) and copies  */
/* them into one contiguous block in pool->children[] once the node is  */
/* fully parsed - after which the block's position no longer moves.     */
/* ------------------------------------------------------------------- */

#define IDXBUF_INLINE 32

typedef struct {
    uint32_t inline_arr[IDXBUF_INLINE];
    uint32_t *arr;   /* == inline_arr until it spills */
    uint32_t count;
    uint32_t cap;
} idxbuf_t;

static void idxbuf_init(idxbuf_t *b) {
    b->arr = b->inline_arr;
    b->count = 0;
    b->cap = IDXBUF_INLINE;
}

static int idxbuf_push(idxbuf_t *b, uint32_t v) {
    if (b->count == b->cap) {
        uint32_t new_cap = b->cap * 2;
        uint32_t *grown;
        if (b->arr == b->inline_arr) {
            grown = (uint32_t *)malloc(new_cap * sizeof(uint32_t));
            if (grown) memcpy(grown, b->inline_arr, b->count * sizeof(uint32_t));
        } else {
            grown = (uint32_t *)realloc(b->arr, new_cap * sizeof(uint32_t));
        }
        if (!grown) return -1;
        b->arr = grown;
        b->cap = new_cap;
    }
    b->arr[b->count++] = v;
    return 0;
}

static void idxbuf_free(idxbuf_t *b) {
    if (b->arr != b->inline_arr) free(b->arr);
}

/* Copies b's contents into a fresh contiguous block of pool->children[]
 * and points child_start/child_count (out params) at it. */
static int idxbuf_commit(idxbuf_t *b, nbt_pool_t *pool, uint32_t *child_start, uint32_t *child_count) {
    if (pool_reserve_children(pool, b->count) != 0) return -1;
    memcpy(pool->children + pool->children_count, b->arr, b->count * sizeof(uint32_t));
    *child_start = pool->children_count;
    *child_count = b->count;
    pool->children_count += b->count;
    return 0;
}

static nbt_result_t parse_name(cursor_t *cur, const char **out_name, uint16_t *out_len) {
    NEED(cur, 2);
    uint16_t len = rd_u16be(cur->base + cur->pos);
    cur->pos += 2;
    NEED(cur, len);
    *out_name = (const char *)(cur->base + cur->pos);
    *out_len = len;
    cur->pos += len;
    return NBT_OK;
}

static nbt_result_t parse_payload(cursor_t *cur, uint8_t type, uint32_t tag_idx, int depth);

/* Parses `count` unnamed payloads of `elem_type` (list elements), pushing
 * each into cur->pool's children array under [child_start, child_start+count). */
static nbt_result_t parse_list_elements(cursor_t *cur, uint8_t elem_type, int32_t count,
                                        int depth, uint32_t parent_idx) {
    if (count < 0) return NBT_ERR_NEGATIVE_LEN;

    idxbuf_t children;
    idxbuf_init(&children);

    for (int32_t i = 0; i < count; i++) {
        uint32_t idx = pool_alloc_tag(cur->pool);
        if (idx == UINT32_MAX) { idxbuf_free(&children); return NBT_ERR_OOM; }
        cur->pool->tags[idx].type = elem_type;
        cur->pool->tags[idx].name = NULL;
        cur->pool->tags[idx].name_len = 0;
        nbt_result_t r = parse_payload(cur, elem_type, idx, depth + 1);
        if (r != NBT_OK) { idxbuf_free(&children); return r; }
        if (idxbuf_push(&children, idx) != 0) { idxbuf_free(&children); return NBT_ERR_OOM; }
    }

    uint32_t child_start, child_count;
    int rc = idxbuf_commit(&children, cur->pool, &child_start, &child_count);
    idxbuf_free(&children);
    if (rc != 0) return NBT_ERR_OOM;

    /* re-fetch pointer: the loop above may have grown pool->tags */
    nbt_tag_t *parent = &cur->pool->tags[parent_idx];
    parent->child_start = child_start;
    parent->child_count = child_count;
    return NBT_OK;
}

/* Parses named tags until TAG_End, pushing each into cur->pool's children
 * array under [child_start, child_start+n) for the compound at parent_idx. */
static nbt_result_t parse_compound_body(cursor_t *cur, int depth, uint32_t parent_idx) {
    idxbuf_t children;
    idxbuf_init(&children);

    for (;;) {
        NEED(cur, 1);
        uint8_t type = cur->base[cur->pos++];
        if (type == NBT_END) break;
        if (type > NBT_LONG_ARRAY) { idxbuf_free(&children); return NBT_ERR_BAD_TAG; }

        const char *name;
        uint16_t name_len;
        nbt_result_t r = parse_name(cur, &name, &name_len);
        if (r != NBT_OK) { idxbuf_free(&children); return r; }

        uint32_t idx = pool_alloc_tag(cur->pool);
        if (idx == UINT32_MAX) { idxbuf_free(&children); return NBT_ERR_OOM; }
        cur->pool->tags[idx].type = type;
        cur->pool->tags[idx].name = name;
        cur->pool->tags[idx].name_len = name_len;

        r = parse_payload(cur, type, idx, depth + 1);
        if (r != NBT_OK) { idxbuf_free(&children); return r; }
        if (idxbuf_push(&children, idx) != 0) { idxbuf_free(&children); return NBT_ERR_OOM; }
    }

    uint32_t child_start, child_count;
    int rc = idxbuf_commit(&children, cur->pool, &child_start, &child_count);
    idxbuf_free(&children);
    if (rc != 0) return NBT_ERR_OOM;

    nbt_tag_t *parent = &cur->pool->tags[parent_idx];
    parent->child_start = child_start;
    parent->child_count = child_count;
    return NBT_OK;
}

static nbt_result_t parse_payload(cursor_t *cur, uint8_t type, uint32_t tag_idx, int depth) {
    if (depth > NBT_MAX_DEPTH) return NBT_ERR_TOO_DEEP;

    switch (type) {
    case NBT_END:
        return NBT_OK;

    case NBT_BYTE: {
        NEED(cur, 1);
        int8_t v = (int8_t)cur->base[cur->pos];
        cur->pos += 1;
        cur->pool->tags[tag_idx].v_byte = v;
        return NBT_OK;
    }
    case NBT_SHORT: {
        NEED(cur, 2);
        int16_t v = (int16_t)rd_u16be(cur->base + cur->pos);
        cur->pos += 2;
        cur->pool->tags[tag_idx].v_short = v;
        return NBT_OK;
    }
    case NBT_INT: {
        NEED(cur, 4);
        int32_t v = (int32_t)rd_u32be(cur->base + cur->pos);
        cur->pos += 4;
        cur->pool->tags[tag_idx].v_int = v;
        return NBT_OK;
    }
    case NBT_LONG: {
        NEED(cur, 8);
        int64_t v = (int64_t)rd_u64be(cur->base + cur->pos);
        cur->pos += 8;
        cur->pool->tags[tag_idx].v_long = v;
        return NBT_OK;
    }
    case NBT_FLOAT: {
        NEED(cur, 4);
        uint32_t bits = rd_u32be(cur->base + cur->pos);
        cur->pos += 4;
        float v;
        memcpy(&v, &bits, 4);
        cur->pool->tags[tag_idx].v_float = v;
        return NBT_OK;
    }
    case NBT_DOUBLE: {
        NEED(cur, 8);
        uint64_t bits = rd_u64be(cur->base + cur->pos);
        cur->pos += 8;
        double v;
        memcpy(&v, &bits, 8);
        cur->pool->tags[tag_idx].v_double = v;
        return NBT_OK;
    }
    case NBT_BYTE_ARRAY: {
        NEED(cur, 4);
        int32_t len = (int32_t)rd_u32be(cur->base + cur->pos);
        cur->pos += 4;
        if (len < 0) return NBT_ERR_NEGATIVE_LEN;
        NEED(cur, len);
        cur->pool->tags[tag_idx].byte_array.len = len;
        cur->pool->tags[tag_idx].byte_array.data = (const int8_t *)(cur->base + cur->pos);
        cur->pos += (size_t)len;
        return NBT_OK;
    }
    case NBT_STRING: {
        const char *s;
        uint16_t slen;
        nbt_result_t r = parse_name(cur, &s, &slen); /* same wire shape as a name */
        if (r != NBT_OK) return r;
        cur->pool->tags[tag_idx].string.len = slen;
        cur->pool->tags[tag_idx].string.data = s;
        return NBT_OK;
    }
    case NBT_LIST: {
        NEED(cur, 5);
        uint8_t elem_type = cur->base[cur->pos];
        int32_t count = (int32_t)rd_u32be(cur->base + cur->pos + 1);
        cur->pos += 5;
        if (elem_type > NBT_LONG_ARRAY) return NBT_ERR_BAD_TAG;
        cur->pool->tags[tag_idx].list_elem_type = elem_type;
        return parse_list_elements(cur, elem_type, count, depth, tag_idx);
    }
    case NBT_COMPOUND:
        return parse_compound_body(cur, depth, tag_idx);

    case NBT_INT_ARRAY: {
        NEED(cur, 4);
        int32_t len = (int32_t)rd_u32be(cur->base + cur->pos);
        cur->pos += 4;
        if (len < 0) return NBT_ERR_NEGATIVE_LEN;
        NEED(cur, (size_t)len * 4);
        uint8_t *p = cur->base + cur->pos;
        bswap32_inplace(p, len);
        cur->pool->tags[tag_idx].int_array.len = len;
        cur->pool->tags[tag_idx].int_array.data = (const int32_t *)p;
        cur->pos += (size_t)len * 4;
        return NBT_OK;
    }
    case NBT_LONG_ARRAY: {
        NEED(cur, 4);
        int32_t len = (int32_t)rd_u32be(cur->base + cur->pos);
        cur->pos += 4;
        if (len < 0) return NBT_ERR_NEGATIVE_LEN;
        NEED(cur, (size_t)len * 8);
        uint8_t *p = cur->base + cur->pos;
        bswap64_inplace(p, len);
        cur->pool->tags[tag_idx].long_array.len = len;
        cur->pool->tags[tag_idx].long_array.data = (const int64_t *)p;
        cur->pos += (size_t)len * 8;
        return NBT_OK;
    }
    default:
        return NBT_ERR_BAD_TAG;
    }
}

nbt_result_t nbt_parse(nbt_pool_t *pool, uint8_t *data, size_t len, nbt_tag_t **out_root) {
    cursor_t cur = { .base = data, .pos = 0, .len = len, .pool = pool };

    NEED(&cur, 1);
    uint8_t type = data[cur.pos++];
    if (type > NBT_LONG_ARRAY) return NBT_ERR_BAD_TAG;

    const char *name = NULL;
    uint16_t name_len = 0;
    if (type != NBT_END) {
        nbt_result_t r = parse_name(&cur, &name, &name_len);
        if (r != NBT_OK) return r;
    }

    uint32_t idx = pool_alloc_tag(pool);
    if (idx == UINT32_MAX) return NBT_ERR_OOM;
    pool->tags[idx].type = type;
    pool->tags[idx].name = name;
    pool->tags[idx].name_len = name_len;

    nbt_result_t r = parse_payload(&cur, type, idx, 0);
    if (r != NBT_OK) return r;

    *out_root = &pool->tags[idx];
    return NBT_OK;
}

/* ------------------------------------------------------------------- */
/* queries / utilities                                                  */
/* ------------------------------------------------------------------- */

nbt_tag_t *nbt_compound_get(const nbt_pool_t *pool, const nbt_tag_t *compound, const char *name, size_t name_len) {
    for (uint32_t i = 0; i < compound->child_count; i++) {
        nbt_tag_t *child = nbt_child_at(pool, compound, i);
        if (child->name_len == name_len && memcmp(child->name, name, name_len) == 0) {
            return child;
        }
    }
    return NULL;
}

nbt_tag_t *nbt_compound_getz(const nbt_pool_t *pool, const nbt_tag_t *compound, const char *name) {
    return nbt_compound_get(pool, compound, name, strlen(name));
}

const char *nbt_type_name(uint8_t type) {
    static const char *names[] = {
        "TAG_End", "TAG_Byte", "TAG_Short", "TAG_Int", "TAG_Long",
        "TAG_Float", "TAG_Double", "TAG_Byte_Array", "TAG_String",
        "TAG_List", "TAG_Compound", "TAG_Int_Array", "TAG_Long_Array",
    };
    if (type > NBT_LONG_ARRAY) return "TAG_Unknown";
    return names[type];
}

uint64_t nbt_count_tags(const nbt_pool_t *pool, const nbt_tag_t *root) {
    uint64_t total = 1;
    if (root->type == NBT_COMPOUND || root->type == NBT_LIST) {
        for (uint32_t i = 0; i < root->child_count; i++) {
            total += nbt_count_tags(pool, nbt_child_at(pool, root, i));
        }
    }
    return total;
}
