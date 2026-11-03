#include "json.h"

#include <stdlib.h>
#include <string.h>

void json_pool_init(json_pool_t *pool) {
    memset(pool, 0, sizeof(*pool));
}

void json_pool_free(json_pool_t *pool) {
    if (!pool) return;
    free(pool->values);
    free(pool->array_children);
    free(pool->object_children);
    free(pool->string_arena);
    memset(pool, 0, sizeof(*pool));
}

static int pool_reserve_values(json_pool_t *pool, uint32_t need) {
    if (pool->value_count + need <= pool->value_cap) return 0;
    uint32_t cap = pool->value_cap ? pool->value_cap * 2 : 256;
    while (cap < pool->value_count + need) cap *= 2;
    json_value_t *grown = (json_value_t *)realloc(pool->values, cap * sizeof(json_value_t));
    if (!grown) return -1;
    pool->values = grown;
    pool->value_cap = cap;
    return 0;
}

static uint32_t pool_alloc_value(json_pool_t *pool) {
    if (pool_reserve_values(pool, 1) != 0) return UINT32_MAX;
    uint32_t idx = pool->value_count++;
    memset(&pool->values[idx], 0, sizeof(json_value_t));
    return idx;
}

/* ------------------------------------------------------------------- */
/* small-vector child accumulation, same pattern as ../nbt/src/nbt.c     */
/* ------------------------------------------------------------------- */

#define IDXBUF_INLINE 16

typedef struct {
    uint32_t inline_arr[IDXBUF_INLINE];
    uint32_t *arr;
    uint32_t count, cap;
} idxbuf_t;

static void idxbuf_init(idxbuf_t *b) { b->arr = b->inline_arr; b->count = 0; b->cap = IDXBUF_INLINE; }
static int idxbuf_push(idxbuf_t *b, uint32_t v) {
    if (b->count == b->cap) {
        uint32_t new_cap = b->cap * 2;
        uint32_t *grown = (b->arr == b->inline_arr)
            ? (uint32_t *)malloc(new_cap * sizeof(uint32_t))
            : (uint32_t *)realloc(b->arr, new_cap * sizeof(uint32_t));
        if (!grown) return -1;
        if (b->arr == b->inline_arr) memcpy(grown, b->inline_arr, b->count * sizeof(uint32_t));
        b->arr = grown;
        b->cap = new_cap;
    }
    b->arr[b->count++] = v;
    return 0;
}
static void idxbuf_free(idxbuf_t *b) { if (b->arr != b->inline_arr) free(b->arr); }

typedef struct {
    json_object_entry_t inline_arr[IDXBUF_INLINE];
    json_object_entry_t *arr;
    uint32_t count, cap;
} entrybuf_t;

static void entrybuf_init(entrybuf_t *b) { b->arr = b->inline_arr; b->count = 0; b->cap = IDXBUF_INLINE; }
static int entrybuf_push(entrybuf_t *b, json_object_entry_t e) {
    if (b->count == b->cap) {
        uint32_t new_cap = b->cap * 2;
        json_object_entry_t *grown = (b->arr == b->inline_arr)
            ? (json_object_entry_t *)malloc(new_cap * sizeof(json_object_entry_t))
            : (json_object_entry_t *)realloc(b->arr, new_cap * sizeof(json_object_entry_t));
        if (!grown) return -1;
        if (b->arr == b->inline_arr) memcpy(grown, b->inline_arr, b->count * sizeof(json_object_entry_t));
        b->arr = grown;
        b->cap = new_cap;
    }
    b->arr[b->count++] = e;
    return 0;
}
static void entrybuf_free(entrybuf_t *b) { if (b->arr != b->inline_arr) free(b->arr); }

/* ------------------------------------------------------------------- */
/* cursor + recursive descent parser                                    */
/* ------------------------------------------------------------------- */

typedef struct {
    const char *base;
    size_t pos, len;
    json_pool_t *pool;
} cursor_t;

static void skip_ws(cursor_t *c) {
    while (c->pos < c->len) {
        char ch = c->base[c->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->pos++;
        else break;
    }
}

static int at_end(cursor_t *c) { return c->pos >= c->len; }
static char peek(cursor_t *c) { return c->base[c->pos]; }

static json_result_t parse_value(cursor_t *c, int depth, uint32_t *out_idx);

static json_result_t parse_literal(cursor_t *c, const char *lit, size_t lit_len) {
    if (c->pos + lit_len > c->len || memcmp(c->base + c->pos, lit, lit_len) != 0) {
        return JSON_ERR_UNEXPECTED_CHAR;
    }
    c->pos += lit_len;
    return JSON_OK;
}

static json_result_t parse_number(cursor_t *c, double *out) {
    size_t start = c->pos;
    if (!at_end(c) && peek(c) == '-') c->pos++;
    while (!at_end(c) && peek(c) >= '0' && peek(c) <= '9') c->pos++;
    if (!at_end(c) && peek(c) == '.') {
        c->pos++;
        while (!at_end(c) && peek(c) >= '0' && peek(c) <= '9') c->pos++;
    }
    if (!at_end(c) && (peek(c) == 'e' || peek(c) == 'E')) {
        c->pos++;
        if (!at_end(c) && (peek(c) == '+' || peek(c) == '-')) c->pos++;
        while (!at_end(c) && peek(c) >= '0' && peek(c) <= '9') c->pos++;
    }
    size_t num_len = c->pos - start;
    if (num_len == 0 || num_len >= 64) return JSON_ERR_BAD_NUMBER;

    char buf[64];
    memcpy(buf, c->base + start, num_len);
    buf[num_len] = '\0';
    char *endptr;
    *out = strtod(buf, &endptr);
    if (endptr != buf + num_len) return JSON_ERR_BAD_NUMBER;
    return JSON_OK;
}

static int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Encodes a unicode code point as UTF-8 into buf, returns byte count. */
static int utf8_encode(uint32_t cp, uint8_t *buf) {
    if (cp < 0x80) { buf[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) {
        buf[0] = (uint8_t)(0xC0 | (cp >> 6));
        buf[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (uint8_t)(0xE0 | (cp >> 12));
        buf[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (uint8_t)(0xF0 | (cp >> 18));
    buf[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a string at its opening quote: a pointer into the source when it
 * has no escapes, else decoded into pool->string_arena. */
static json_result_t parse_string_raw(cursor_t *c, const char **out_ptr, uint32_t *out_len) {
    if (at_end(c) || peek(c) != '"') return JSON_ERR_UNEXPECTED_CHAR;
    c->pos++;
    size_t start = c->pos;
    int has_escape = 0;

    while (!at_end(c) && peek(c) != '"') {
        if (peek(c) == '\\') { has_escape = 1; c->pos++; if (at_end(c)) return JSON_ERR_TRUNCATED; }
        c->pos++;
    }
    if (at_end(c)) return JSON_ERR_TRUNCATED;
    size_t raw_len = c->pos - start;
    c->pos++; /* closing quote */

    if (!has_escape) {
        *out_ptr = c->base + start;
        *out_len = (uint32_t)raw_len;
        return JSON_OK;
    }

    /* slow path: decode into the arena, writing at most raw_len bytes */
    size_t arena_start = c->pool->string_arena_len;
    if (c->pool->string_arena_len + raw_len > c->pool->string_arena_cap) {
        size_t cap = c->pool->string_arena_cap ? c->pool->string_arena_cap * 2 : 4096;
        while (cap < c->pool->string_arena_len + raw_len) cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(c->pool->string_arena, cap);
        if (!grown) return JSON_ERR_OOM;
        c->pool->string_arena = grown;
        c->pool->string_arena_cap = cap;
    }
    uint8_t *dst = c->pool->string_arena + arena_start;
    size_t dst_len = 0;

    size_t p = start;
    size_t raw_end = start + raw_len;
    while (p < raw_end) {
        char ch = c->base[p];
        if (ch != '\\') { dst[dst_len++] = (uint8_t)ch; p++; continue; }
        p++;
        char esc = c->base[p++];
        switch (esc) {
        case '"': dst[dst_len++] = '"'; break;
        case '\\': dst[dst_len++] = '\\'; break;
        case '/': dst[dst_len++] = '/'; break;
        case 'b': dst[dst_len++] = '\b'; break;
        case 'f': dst[dst_len++] = '\f'; break;
        case 'n': dst[dst_len++] = '\n'; break;
        case 'r': dst[dst_len++] = '\r'; break;
        case 't': dst[dst_len++] = '\t'; break;
        case 'u': {
            if (p + 4 > raw_end) return JSON_ERR_BAD_ESCAPE;
            uint32_t cp = 0;
            for (int i = 0; i < 4; i++) {
                int d = hex_digit(c->base[p + i]);
                if (d < 0) return JSON_ERR_BAD_ESCAPE;
                cp = (cp << 4) | (uint32_t)d;
            }
            p += 4;
            dst_len += (size_t)utf8_encode(cp, dst + dst_len);
            break;
        }
        default:
            return JSON_ERR_BAD_ESCAPE;
        }
    }

    c->pool->string_arena_len += raw_len; /* reserve worst case, dst_len <= raw_len always */
    *out_ptr = (const char *)dst;
    *out_len = (uint32_t)dst_len;
    return JSON_OK;
}

static json_result_t parse_array(cursor_t *c, int depth, uint32_t idx) {
    c->pos++; /* '[' */
    idxbuf_t children;
    idxbuf_init(&children);

    skip_ws(c);
    if (!at_end(c) && peek(c) == ']') {
        c->pos++;
    } else {
        for (;;) {
            skip_ws(c);
            uint32_t child_idx;
            json_result_t r = parse_value(c, depth + 1, &child_idx);
            if (r != JSON_OK) { idxbuf_free(&children); return r; }
            if (idxbuf_push(&children, child_idx) != 0) { idxbuf_free(&children); return JSON_ERR_OOM; }
            skip_ws(c);
            if (at_end(c)) { idxbuf_free(&children); return JSON_ERR_TRUNCATED; }
            if (peek(c) == ',') { c->pos++; continue; }
            if (peek(c) == ']') { c->pos++; break; }
            idxbuf_free(&children);
            return JSON_ERR_UNEXPECTED_CHAR;
        }
    }

    json_pool_t *pool = c->pool;
    if (pool->array_children_count + children.count > pool->array_children_cap) {
        uint32_t cap = pool->array_children_cap ? pool->array_children_cap * 2 : 256;
        while (cap < pool->array_children_count + children.count) cap *= 2;
        uint32_t *grown = (uint32_t *)realloc(pool->array_children, cap * sizeof(uint32_t));
        if (!grown) { idxbuf_free(&children); return JSON_ERR_OOM; }
        pool->array_children = grown;
        pool->array_children_cap = cap;
    }
    uint32_t start = pool->array_children_count;
    memcpy(pool->array_children + start, children.arr, children.count * sizeof(uint32_t));
    pool->array_children_count += children.count;
    idxbuf_free(&children);

    pool->values[idx].array.start = start;
    pool->values[idx].array.count = (uint32_t)(pool->array_children_count - start);
    return JSON_OK;
}

static json_result_t parse_object(cursor_t *c, int depth, uint32_t idx) {
    c->pos++; /* '{' */
    entrybuf_t children;
    entrybuf_init(&children);

    skip_ws(c);
    if (!at_end(c) && peek(c) == '}') {
        c->pos++;
    } else {
        for (;;) {
            skip_ws(c);
            const char *key_ptr;
            uint32_t key_len;
            json_result_t r = parse_string_raw(c, &key_ptr, &key_len);
            if (r != JSON_OK) { entrybuf_free(&children); return r; }
            skip_ws(c);
            if (at_end(c) || peek(c) != ':') { entrybuf_free(&children); return JSON_ERR_UNEXPECTED_CHAR; }
            c->pos++;
            skip_ws(c);

            uint32_t value_idx;
            r = parse_value(c, depth + 1, &value_idx);
            if (r != JSON_OK) { entrybuf_free(&children); return r; }

            json_object_entry_t entry = { key_ptr, key_len, value_idx };
            if (entrybuf_push(&children, entry) != 0) { entrybuf_free(&children); return JSON_ERR_OOM; }

            skip_ws(c);
            if (at_end(c)) { entrybuf_free(&children); return JSON_ERR_TRUNCATED; }
            if (peek(c) == ',') { c->pos++; continue; }
            if (peek(c) == '}') { c->pos++; break; }
            entrybuf_free(&children);
            return JSON_ERR_UNEXPECTED_CHAR;
        }
    }

    json_pool_t *pool = c->pool;
    if (pool->object_children_count + children.count > pool->object_children_cap) {
        uint32_t cap = pool->object_children_cap ? pool->object_children_cap * 2 : 256;
        while (cap < pool->object_children_count + children.count) cap *= 2;
        json_object_entry_t *grown = (json_object_entry_t *)realloc(
            pool->object_children, cap * sizeof(json_object_entry_t));
        if (!grown) { entrybuf_free(&children); return JSON_ERR_OOM; }
        pool->object_children = grown;
        pool->object_children_cap = cap;
    }
    uint32_t start = pool->object_children_count;
    memcpy(pool->object_children + start, children.arr, children.count * sizeof(json_object_entry_t));
    pool->object_children_count += children.count;
    entrybuf_free(&children);

    pool->values[idx].object.start = start;
    pool->values[idx].object.count = (uint32_t)(pool->object_children_count - start);
    return JSON_OK;
}

static json_result_t parse_value(cursor_t *c, int depth, uint32_t *out_idx) {
    if (depth > JSON_MAX_DEPTH) return JSON_ERR_TOO_DEEP;
    skip_ws(c);
    if (at_end(c)) return JSON_ERR_TRUNCATED;

    uint32_t idx = pool_alloc_value(c->pool);
    if (idx == UINT32_MAX) return JSON_ERR_OOM;
    *out_idx = idx;

    char ch = peek(c);
    json_result_t r;
    switch (ch) {
    case '{':
        c->pool->values[idx].type = JSON_OBJECT;
        return parse_object(c, depth, idx);
    case '[':
        c->pool->values[idx].type = JSON_ARRAY;
        return parse_array(c, depth, idx);
    case '"': {
        const char *ptr; uint32_t len;
        r = parse_string_raw(c, &ptr, &len);
        if (r != JSON_OK) return r;
        c->pool->values[idx].type = JSON_STRING;
        c->pool->values[idx].string.ptr = ptr;
        c->pool->values[idx].string.len = len;
        return JSON_OK;
    }
    case 't':
        r = parse_literal(c, "true", 4);
        if (r != JSON_OK) return r;
        c->pool->values[idx].type = JSON_TRUE;
        return JSON_OK;
    case 'f':
        r = parse_literal(c, "false", 5);
        if (r != JSON_OK) return r;
        c->pool->values[idx].type = JSON_FALSE;
        return JSON_OK;
    case 'n':
        r = parse_literal(c, "null", 4);
        if (r != JSON_OK) return r;
        c->pool->values[idx].type = JSON_NULL;
        return JSON_OK;
    default:
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            double num;
            r = parse_number(c, &num);
            if (r != JSON_OK) return r;
            c->pool->values[idx].type = JSON_NUMBER;
            c->pool->values[idx].number = num;
            return JSON_OK;
        }
        return JSON_ERR_UNEXPECTED_CHAR;
    }
}

json_result_t json_parse(json_pool_t *pool, const char *data, size_t len, json_value_t **out_root) {
    cursor_t c = { data, 0, len, pool };
    uint32_t idx;
    json_result_t r = parse_value(&c, 0, &idx);
    if (r != JSON_OK) return r;
    skip_ws(&c);
    *out_root = &pool->values[idx];
    return JSON_OK;
}

json_value_t *json_object_get(const json_pool_t *pool, const json_value_t *obj, const char *key, size_t key_len) {
    for (uint32_t i = 0; i < obj->object.count; i++) {
        const json_object_entry_t *e = &pool->object_children[obj->object.start + i];
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            return &pool->values[e->value];
        }
    }
    return NULL;
}

json_value_t *json_object_getz(const json_pool_t *pool, const json_value_t *obj, const char *key) {
    return json_object_get(pool, obj, key, strlen(key));
}

const char *json_object_key_at(const json_pool_t *pool, const json_value_t *obj, uint32_t i, uint32_t *out_len) {
    const json_object_entry_t *e = &pool->object_children[obj->object.start + i];
    *out_len = e->key_len;
    return e->key;
}

json_value_t *json_object_value_at(const json_pool_t *pool, const json_value_t *obj, uint32_t i) {
    const json_object_entry_t *e = &pool->object_children[obj->object.start + i];
    return &pool->values[e->value];
}
