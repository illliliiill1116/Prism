/*
 * faces.c - the flat six-face resolvers: blockmodel_resolve (by name,
 * first variant) and blockmodel_resolve_state (variant matching the
 * props). "multipart" is unsupported here; shape.c handles real geometry.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* A chosen variant value ({"model":...} or an array) down to six face
 * textures. `chosen` belongs to `pool`. */
static blockmodel_result_t variant_to_faces(blockmodel_registry_t *reg, const json_pool_t *pool,
                                             const json_value_t *chosen, block_faces_t *out) {
    if (chosen->type == JSON_ARRAY) {
        if (json_array_count(chosen) == 0) return BLOCKMODEL_ERR_UNSUPPORTED;
        chosen = json_array_at(pool, chosen, 0);
    }
    const json_value_t *model_val = (chosen->type == JSON_OBJECT) ? json_object_getz(pool, chosen, "model") : NULL;
    if (!model_val || model_val->type != JSON_STRING || model_val->string.len >= MAX_PATH_LEN)
        return BLOCKMODEL_ERR_UNSUPPORTED;

    char model_ref[MAX_PATH_LEN];
    memcpy(model_ref, model_val->string.ptr, model_val->string.len);
    size_t model_ref_len = model_val->string.len;
    model_ref[model_ref_len] = '\0';

    texture_var_t vars[MAX_TEXTURE_VARS];
    uint32_t var_count = 0;
    face_state_t face_states[FACE_COUNT];
    memset(face_states, 0, sizeof(face_states));

    blockmodel_result_t r = bm_resolve_model_chain(reg, model_ref, model_ref_len, vars, &var_count, face_states);
    if (r != BLOCKMODEL_OK) return r;

    memset(out, 0, sizeof(*out));
    for (int f = 0; f < FACE_COUNT; f++) {
        out->tint[f] = face_states[f].tint;
        if (!face_states[f].resolved) continue;
        char literal[MAX_VAR_VALUE];
        memcpy(literal, face_states[f].var_or_literal, sizeof(literal));
        if (bm_resolve_var_chain(vars, var_count, literal, sizeof(literal)) != 0) continue;
        out->texture[f] = bm_load_texture_cached(reg, literal, strlen(literal));
    }
    return BLOCKMODEL_OK;
}

/* See blockmodel.h. Not cached here. */
blockmodel_result_t blockmodel_resolve_state(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                                             const blockmodel_property_t *props, size_t prop_count,
                                             block_faces_t *out_faces) {
    uint8_t *data;
    json_pool_t pool;
    json_value_t *root;
    blockmodel_result_t lr = bm_load_json(reg, "blockstates/", block_name, name_len, &data, &pool, &root);
    if (lr == BLOCKMODEL_ERR_UNSUPPORTED) return BLOCKMODEL_ERR_NOT_FOUND;
    if (lr != BLOCKMODEL_OK) return lr;

    json_value_t *variants = json_object_getz(&pool, root, "variants");
    if (!variants || variants->type != JSON_OBJECT || json_object_count(variants) == 0) {
        bm_free_json(data, &pool);
        return BLOCKMODEL_ERR_UNSUPPORTED; /* multipart - shape.c territory */
    }

    json_value_t *chosen = NULL;
    for (uint32_t i = 0; i < json_object_count(variants); i++) {
        uint32_t klen;
        const char *k = json_object_key_at(&pool, variants, i, &klen);
        if (bm_variant_key_matches(k, klen, props, prop_count)) {
            chosen = json_object_value_at(&pool, variants, i);
            break;
        }
    }
    if (!chosen) chosen = json_object_value_at(&pool, variants, 0);

    blockmodel_result_t r = variant_to_faces(reg, &pool, chosen, out_faces);
    bm_free_json(data, &pool);
    return r;
}

/* Read-only probe of the open-addressed faces_index - same shape as
 * texture_probe in registry.c, see its comment. */
static const block_faces_t *faces_probe(blockmodel_registry_t *reg, const char *name, size_t name_len,
                                        uint32_t *inout_slot) {
    uint32_t slot = *inout_slot;
    while (reg->faces_index[slot]) {
        faces_cache_entry_t *hit = &reg->faces_cache[reg->faces_index[slot] - 1];
        if (hit->key_len == name_len && memcmp(hit->key, name, name_len) == 0) return &hit->faces;
        slot = (slot + 1) & (FACES_INDEX_SLOTS - 1);
    }
    *inout_slot = slot;
    return NULL;
}

blockmodel_result_t blockmodel_resolve
    (
        blockmodel_registry_t *reg,
        const char *block_name, size_t name_len,
        const block_faces_t **out_faces
    )
{
    uint32_t slot0 = bm_key_hash(block_name, name_len) & (FACES_INDEX_SLOTS - 1);
    uint32_t shard = prism_get_shard_id();

    uint32_t slot = slot0;
    percpu_rwlock_rdlock(&reg->lock, shard);
    const block_faces_t *hit = faces_probe(reg, block_name, name_len, &slot);
    percpu_rwlock_rdunlock(&reg->lock, shard);
    if (hit) { *out_faces = hit; return BLOCKMODEL_OK; }

    /* Resolve outside the lock; texture lookups lock briefly on their
     * own. */
    uint8_t *data;
    json_pool_t pool;
    json_value_t *root;
    blockmodel_result_t lr = bm_load_json(reg, "blockstates/", block_name, name_len, &data, &pool, &root);
    if (lr == BLOCKMODEL_ERR_UNSUPPORTED) return BLOCKMODEL_ERR_NOT_FOUND; /* over-long name: same as before */
    if (lr != BLOCKMODEL_OK) return lr;

    json_value_t *variants = json_object_getz(&pool, root, "variants");
    if (!variants || variants->type != JSON_OBJECT || json_object_count(variants) == 0) {
        bm_free_json(data, &pool);
        return BLOCKMODEL_ERR_UNSUPPORTED; /* "multipart" blockstates (fences, walls, redstone wire, ...) */
    }

    json_value_t *chosen = json_object_value_at(&pool, variants, 0);
    block_faces_t faces;
    blockmodel_result_t r = variant_to_faces(reg, &pool, chosen, &faces);
    bm_free_json(data, &pool);
    if (r != BLOCKMODEL_OK) return r;

    percpu_rwlock_wrlock(&reg->lock);
    slot = slot0;
    hit = faces_probe(reg, block_name, name_len, &slot); /* re-check: another thread may have inserted */
    if (hit) {
        *out_faces = hit;
        percpu_rwlock_wrunlock(&reg->lock);
        return BLOCKMODEL_OK;
    }
    if (reg->faces_count >= BLOCKMODEL_MAX_BLOCKS) {
        percpu_rwlock_wrunlock(&reg->lock);
        return BLOCKMODEL_ERR_OOM;
    }
    faces_cache_entry_t *newslot = &reg->faces_cache[reg->faces_count];
    newslot->key = (char *)malloc(name_len);
    if (!newslot->key) {
        percpu_rwlock_wrunlock(&reg->lock);
        return BLOCKMODEL_ERR_OOM;
    }
    memcpy(newslot->key, block_name, name_len);
    newslot->key_len = name_len;
    newslot->faces = faces;
    reg->faces_index[slot] = reg->faces_count + 1;
    reg->faces_count++;
    *out_faces = &newslot->faces;
    percpu_rwlock_wrunlock(&reg->lock);
    return BLOCKMODEL_OK;
}
