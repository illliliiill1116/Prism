/*
 * registry.c - open/close, cache storage, and the jar primitives the
 * resolvers share: asset paths, JSON loading, the texture cache,
 * texture-variable chains.
 */
#include "internal.h"
#include "png_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const bm_face_names[FACE_COUNT] = {"up", "down", "north", "south", "east", "west"};

/* ------------------------------------------------------------------- */
/* path helpers                                                         */
/* ------------------------------------------------------------------- */

static void strip_namespace(const char *ref, size_t ref_len, const char **out, size_t *out_len) {
    const char *colon = (const char *)memchr(ref, ':', ref_len);
    if (colon) {
        size_t skip = (size_t)(colon - ref) + 1;
        *out = ref + skip;
        *out_len = ref_len - skip;
    } else {
        *out = ref;
        *out_len = ref_len;
    }
}

int bm_build_asset_path(char *buf, size_t buf_cap, const char *category,
                        const char *ref, size_t ref_len, const char *ext) {
    const char *local;
    size_t local_len;
    strip_namespace(ref, ref_len, &local, &local_len);
    int n = snprintf(buf, buf_cap, "assets/minecraft/%s%.*s%s", category, (int)local_len, local, ext);
    return (n > 0 && (size_t)n < buf_cap) ? 0 : -1;
}

/* ------------------------------------------------------------------- */
/* JSON asset loading                                                   */
/* ------------------------------------------------------------------- */

blockmodel_result_t bm_load_json(blockmodel_registry_t *reg, const char *category,
                                 const char *ref, size_t ref_len,
                                 uint8_t **out_data, json_pool_t *out_pool, json_value_t **out_root) {
    char path[MAX_PATH_LEN];
    if (bm_build_asset_path(path, sizeof(path), category, ref, ref_len, ".json") != 0) {
        return BLOCKMODEL_ERR_UNSUPPORTED;
    }
    const zip_entry_t *e = zip_findz(&reg->jar, path);
    if (!e) return BLOCKMODEL_ERR_NOT_FOUND;

    uint8_t *data;
    size_t data_len;
    if (zip_extract(&reg->jar, e, &data, &data_len) != ZIP_OK) return BLOCKMODEL_ERR_IO;

    json_pool_init(out_pool);
    if (json_parse(out_pool, (const char *)data, data_len, out_root) != JSON_OK) {
        json_pool_free(out_pool);
        free(data);
        return BLOCKMODEL_ERR_IO;
    }
    /* data must stay alive until bm_free_json: zero-copy JSON strings
     * (the common case, no escape sequences) point directly into it */
    *out_data = data;
    return BLOCKMODEL_OK;
}

void bm_free_json(uint8_t *data, json_pool_t *pool) {
    json_pool_free(pool);
    free(data);
}

/* ------------------------------------------------------------------- */
/* texture cache                                                        */
/* ------------------------------------------------------------------- */

uint32_t bm_key_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

/* Read-locked probe of texture_index. *inout_slot ends at the first empty
 * slot on a miss; the writer re-probes before trusting it. */
static const canvas_t *texture_probe(blockmodel_registry_t *reg, const char *path, size_t path_len,
                                     uint32_t *inout_slot) {
    uint32_t slot = *inout_slot;
    while (reg->texture_index[slot]) {
        texture_cache_entry_t *hit = &reg->textures[reg->texture_index[slot] - 1];
        if (hit->key_len == path_len && memcmp(hit->key, path, path_len) == 0) return &hit->image;
        slot = (slot + 1) & (TEXTURE_INDEX_SLOTS - 1);
    }
    *inout_slot = slot;
    return NULL;
}

const canvas_t *bm_load_texture_cached(blockmodel_registry_t *reg, const char *ref, size_t ref_len) {
    char path[MAX_PATH_LEN];
    if (bm_build_asset_path(path, sizeof(path), "textures/", ref, ref_len, ".png") != 0) return NULL;
    size_t path_len = strlen(path);
    uint32_t slot0 = bm_key_hash(path, path_len) & (TEXTURE_INDEX_SLOTS - 1);

    uint32_t shard = prism_get_shard_id();
    uint32_t slot = slot0;
    percpu_rwlock_rdlock(&reg->lock, shard);
    const canvas_t *hit = texture_probe(reg, path, path_len, &slot);
    percpu_rwlock_rdunlock(&reg->lock, shard);
    if (hit) return hit;

    /* Miss: extract and decode outside the lock (concurrent misses may
     * decode twice); only the insert takes the write lock. */
    const zip_entry_t *e = zip_find(&reg->jar, path, path_len);
    if (!e) return NULL;
    uint8_t *data;
    size_t data_len;
    if (zip_extract(&reg->jar, e, &data, &data_len) != ZIP_OK) return NULL;

    canvas_t img;
    png_result_t pr = png_decode(data, data_len, &img);
    free(data);
    if (pr != PNG_OK) return NULL;
    return bm_texture_put(reg, path, path_len, &img);
}

const canvas_t *bm_texture_get(blockmodel_registry_t *reg, const char *key, size_t key_len) {
    uint32_t shard = prism_get_shard_id();
    uint32_t slot = bm_key_hash(key, key_len) & (TEXTURE_INDEX_SLOTS - 1);
    percpu_rwlock_rdlock(&reg->lock, shard);
    const canvas_t *hit = texture_probe(reg, key, key_len, &slot);
    percpu_rwlock_rdunlock(&reg->lock, shard);
    return hit;
}

const canvas_t *bm_texture_put(blockmodel_registry_t *reg, const char *path, size_t path_len, canvas_t *imgp) {
    canvas_t img = *imgp;
    uint32_t slot0 = bm_key_hash(path, path_len) & (TEXTURE_INDEX_SLOTS - 1), slot;
    const canvas_t *hit;
    percpu_rwlock_wrlock(&reg->lock);
    slot = slot0;
    hit = texture_probe(reg, path, path_len, &slot); /* another thread may have inserted meanwhile */
    if (hit) {
        percpu_rwlock_wrunlock(&reg->lock);
        canvas_free(&img);
        return hit;
    }
    if (reg->texture_count >= BLOCKMODEL_MAX_TEXTURES) {
        percpu_rwlock_wrunlock(&reg->lock);
        canvas_free(&img);
        return NULL;
    }
    texture_cache_entry_t *newslot = &reg->textures[reg->texture_count];
    newslot->key = (char *)malloc(path_len);
    if (!newslot->key) {
        percpu_rwlock_wrunlock(&reg->lock);
        canvas_free(&img);
        return NULL;
    }
    memcpy(newslot->key, path, path_len);
    newslot->key_len = path_len;
    newslot->image = img;
    reg->texture_index[slot] = reg->texture_count + 1;
    reg->texture_count++;
    const canvas_t *out = &newslot->image;
    percpu_rwlock_wrunlock(&reg->lock);
    return out;
}

const canvas_t *blockmodel_texture(blockmodel_registry_t *reg, const char *ref, size_t ref_len) {
    return bm_load_texture_cached(reg, ref, ref_len);
}

int bm_resolve_var_chain(const texture_var_t *vars, uint32_t var_count, char *inout, size_t inout_cap) {
    for (int hop = 0; hop < 8; hop++) {
        if (inout[0] != '#') return 0;
        const char *name = inout + 1;
        size_t name_len = strlen(name);
        int found = 0;
        for (uint32_t v = 0; v < var_count; v++) {
            if (strlen(vars[v].name) == name_len && memcmp(vars[v].name, name, name_len) == 0) {
                size_t vlen = strlen(vars[v].value);
                if (vlen >= inout_cap) return -1;
                memcpy(inout, vars[v].value, vlen + 1);
                found = 1;
                break;
            }
        }
        if (!found) return -1;
    }
    return -1;
}

/* ------------------------------------------------------------------- */
/* open/close                                                           */
/* ------------------------------------------------------------------- */

blockmodel_result_t blockmodel_open(const char *jar_path, blockmodel_registry_t **out_reg) {
    blockmodel_registry_t *reg = (blockmodel_registry_t *)calloc(1, sizeof(blockmodel_registry_t));
    if (!reg) return BLOCKMODEL_ERR_OOM;

    if (zip_open(jar_path, &reg->jar) != ZIP_OK) { free(reg); return BLOCKMODEL_ERR_IO; }

    reg->textures = (texture_cache_entry_t *)calloc(BLOCKMODEL_MAX_TEXTURES, sizeof(texture_cache_entry_t));
    reg->texture_index = (uint32_t *)calloc(TEXTURE_INDEX_SLOTS, sizeof(uint32_t));
    reg->faces_cache = (faces_cache_entry_t *)calloc(BLOCKMODEL_MAX_BLOCKS, sizeof(faces_cache_entry_t));
    reg->faces_index = (uint32_t *)calloc(FACES_INDEX_SLOTS, sizeof(uint32_t));
    reg->shapes = (shape_cache_entry_t *)calloc(BLOCKMODEL_MAX_SHAPES, sizeof(shape_cache_entry_t));
    reg->shape_index = (uint32_t *)calloc(SHAPE_INDEX_SLOTS, sizeof(uint32_t));
    if (!reg->textures || !reg->texture_index || !reg->faces_cache || !reg->faces_index ||
        !reg->shapes || !reg->shape_index) {
        blockmodel_close(reg);
        return BLOCKMODEL_ERR_OOM;
    }
    /* One shard until blockmodel_set_shards. */
    if (!percpu_rwlock_init(&reg->lock, 1)) {
        blockmodel_close(reg);
        return BLOCKMODEL_ERR_OOM;
    }

    *out_reg = reg;
    return BLOCKMODEL_OK;
}

int blockmodel_set_shards(blockmodel_registry_t *reg, uint32_t nshards) {
    percpu_rwlock_destroy(&reg->lock);
    return percpu_rwlock_init(&reg->lock, nshards);
}

void blockmodel_close(blockmodel_registry_t *reg) {
    if (!reg) return;
    if (reg->textures) {
        for (uint32_t i = 0; i < reg->texture_count; i++) {
            free(reg->textures[i].key);
            canvas_free(&reg->textures[i].image);
        }
        free(reg->textures);
    }
    free(reg->texture_index);
    if (reg->faces_cache) {
        for (uint32_t i = 0; i < reg->faces_count; i++) free(reg->faces_cache[i].key);
        free(reg->faces_cache);
    }
    free(reg->faces_index);
    if (reg->shapes) {
        for (uint32_t i = 0; i < reg->shape_count; i++) {
            free(reg->shapes[i].key);
            free(reg->shapes[i].shape.elements);
        }
        free(reg->shapes);
    }
    free(reg->shape_index);
    percpu_rwlock_destroy(&reg->lock);
    zip_close(&reg->jar);
    free(reg);
}
