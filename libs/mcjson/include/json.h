/*
 * json.h - a minimal JSON parser for Minecraft's blockstate and model
 * files; not a general-purpose library (see README.md for the subset).
 *
 * As in ../nbt, values live in one flat growable array referenced by index
 * (stable across realloc). Object/array children are contiguous index
 * ranges in a second array, committed after each node's children are
 * parsed.
 *
 * Strings point into the source buffer when they have no escapes, and are
 * decoded into a scratch arena when they do.
 */
#ifndef PRISM_JSON_H
#define PRISM_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum json_type {
    JSON_NULL = 0,
    JSON_FALSE,
    JSON_TRUE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_type_t;

typedef struct json_value {
    json_type_t type;
    union {
        double number;
        struct { const char *ptr; uint32_t len; } string; /* not null-terminated */
        struct { uint32_t start, count; } array;  /* index range into pool->array_children */
        struct { uint32_t start, count; } object; /* index range into pool->object_children */
    };
} json_value_t;

typedef struct json_object_entry {
    const char *key;
    uint32_t key_len;
    uint32_t value; /* index into pool->values */
} json_object_entry_t;

typedef struct json_pool {
    json_value_t *values;
    uint32_t value_count, value_cap;

    uint32_t *array_children; /* value indices */
    uint32_t array_children_count, array_children_cap;

    json_object_entry_t *object_children;
    uint32_t object_children_count, object_children_cap;

    uint8_t *string_arena; /* owns decoded (escaped) string bytes */
    size_t string_arena_len, string_arena_cap;
} json_pool_t;

typedef enum json_result {
    JSON_OK = 0,
    JSON_ERR_UNEXPECTED_CHAR,
    JSON_ERR_TRUNCATED,
    JSON_ERR_TOO_DEEP,
    JSON_ERR_BAD_NUMBER,
    JSON_ERR_BAD_ESCAPE,
    JSON_ERR_OOM,
} json_result_t;

#ifndef JSON_MAX_DEPTH
#define JSON_MAX_DEPTH 256
#endif

void json_pool_init(json_pool_t *pool);
void json_pool_free(json_pool_t *pool);

/* `data` need not be null-terminated and isn't modified, but strings may
 * point into it, so keep it alive while the pool is used. */
json_result_t json_parse(json_pool_t *pool, const char *data, size_t len, json_value_t **out_root);

static inline json_value_t *json_array_at(const json_pool_t *pool, const json_value_t *arr, uint32_t i) {
    return &pool->values[pool->array_children[arr->array.start + i]];
}
static inline uint32_t json_array_count(const json_value_t *arr) { return arr->array.count; }

/* Linear search (object sizes here are small - a handful of keys). */
json_value_t *json_object_get(const json_pool_t *pool, const json_value_t *obj, const char *key, size_t key_len);
json_value_t *json_object_getz(const json_pool_t *pool, const json_value_t *obj, const char *key);

static inline uint32_t json_object_count(const json_value_t *obj) { return obj->object.count; }
/* For iterating dynamic keys (e.g. blockstate "variants" keyed by
 * property strings like "facing=east,half=bottom"). */
const char *json_object_key_at(const json_pool_t *pool, const json_value_t *obj, uint32_t i, uint32_t *out_len);
json_value_t *json_object_value_at(const json_pool_t *pool, const json_value_t *obj, uint32_t i);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_JSON_H */
