/*
 * shape.c - blockmodel_resolve_shape: variant matching, multipart "when"
 * evaluation, whole-model x/y rotation, and the per-state shape cache.
 *
 * Rotation by k*90 degrees applies x first, then y, to geometry and face
 * directions. Y cycles NORTH->EAST->SOUTH->WEST per +90 (oak_stairs'
 * facing=north is y=270 of the east-facing model). X cycles
 * UP->NORTH->DOWN->SOUTH per +90, the same clockwise-from-positive-axis
 * convention; it is less checked against jar data than Y.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- */
/* rotation                                                             */
/* ------------------------------------------------------------------- */

static block_face_dir_t rotate_face_y(block_face_dir_t d, int k) {
    if (d == FACE_UP || d == FACE_DOWN) return d;
    static const block_face_dir_t cyc[4] = {FACE_NORTH, FACE_EAST, FACE_SOUTH, FACE_WEST};
    int idx = 0;
    for (int i = 0; i < 4; i++) if (cyc[i] == d) { idx = i; break; }
    return cyc[(idx + k) & 3];
}

static block_face_dir_t rotate_face_x(block_face_dir_t d, int k) {
    if (d == FACE_EAST || d == FACE_WEST) return d;
    static const block_face_dir_t cyc[4] = {FACE_UP, FACE_NORTH, FACE_DOWN, FACE_SOUTH};
    int idx = 0;
    for (int i = 0; i < 4; i++) if (cyc[i] == d) { idx = i; break; }
    return cyc[(idx + k) & 3];
}

/* x rotation first (touches y,z; x fixed), then y rotation (touches
 * x,z; y fixed) on the result - the documented application order. */
static void transform_point(double x, double y, double z, int kx, int ky, double *ox, double *oy, double *oz) {
    double y1 = y, z1 = z;
    for (int i = 0; i < kx; i++) { double ny = z1, nz = 16.0 - y1; y1 = ny; z1 = nz; }
    double x2 = x, z2 = z1;
    for (int i = 0; i < ky; i++) { double nx = 16.0 - z2, nz = x2; x2 = nx; z2 = nz; }
    *ox = x2; *oy = y1; *oz = z2;
}

/* transform_point's rotation for a direction vector (no translation). */
static void transform_dir(double x, double y, double z, int kx, int ky, double *ox, double *oy, double *oz) {
    double y1 = y, z1 = z;
    for (int i = 0; i < kx; i++) { double ny = z1, nz = -y1; y1 = ny; z1 = nz; }
    double x2 = x, z2 = z1;
    for (int i = 0; i < ky; i++) { double nx = -z2, nz = x2; x2 = nx; z2 = nz; }
    *ox = x2; *oy = y1; *oz = z2;
}

/* Each face's texture axes as the game lays the uv rectangle on it (v runs
 * down the sides): up u=+x v=+z, down u=+x v=-z, north u=-x v=-y, south
 * u=+x v=-y, east u=-z v=-y, west u=+z v=-y. All six have the same
 * handedness, so a quarter-turn model rotation always maps one face's axes
 * onto another's by a rotation - never the reflection that left an x/y
 * rotated chain (axis=x) with its texture across the plane instead of along. */
static void face_uv_basis(int dir, double U[3], double V[3]) {
    static const double u[6][3] = {{1,0,0},{1,0,0},{-1,0,0},{1,0,0},{0,0,-1},{0,0,1}};
    static const double v[6][3] = {{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0},{0,-1,0},{0,-1,0}};
    for (int i = 0; i < 3; i++) { U[i] = u[dir][i]; V[i] = v[dir][i]; }
}

/* How far the model's x/y rotation spins the texture on a face moving from
 * `fdir` to `d3`: rotate the source u/v axes and compare with the
 * destination's. Always 0/90/180/270. */
static int veq3(const double a[3], const double b[3]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}
static int induced_uvrot(int fdir, int kx, int ky, int d3) {
    double U[3], V[3];
    face_uv_basis(fdir, U, V);
    double tU[3], tV[3];
    transform_dir(U[0], U[1], U[2], kx, ky, &tU[0], &tU[1], &tU[2]);
    transform_dir(V[0], V[1], V[2], kx, ky, &tV[0], &tV[1], &tV[2]);
    double Uc[3], Vc[3];
    face_uv_basis(d3, Uc, Vc);
    double nUc[3] = {-Uc[0], -Uc[1], -Uc[2]}, nVc[3] = {-Vc[0], -Vc[1], -Vc[2]};
    if (veq3(tU, Uc)  && veq3(tV, Vc))  return 0;
    if (veq3(tU, Vc)  && veq3(tV, nUc)) return 90;
    if (veq3(tU, nUc) && veq3(tV, nVc)) return 180;
    if (veq3(tU, nVc) && veq3(tV, Uc))  return 270;
    return 0; /* unexpected (e.g. reflection) - leave unrotated */
}

/* Composes an element's own rotation with the model's x/y rotation into
 * `oe`. The game applies element rotation first; a quarter-turn model
 * rotation keeps the angle, maps the axis (negated if it flips) and the
 * origin. */
static void compose_element_rotation(const raw_element_t *re, int kx, int ky, blockmodel_element_t *oe) {
    oe->has_xf = 0;
    if (re->rot_angle == 0.0) {
        oe->rot_axis = 0; oe->rot_angle = 0.0; oe->rot_rescale = 0;
        oe->rot_origin[0] = oe->rot_origin[1] = oe->rot_origin[2] = 8.0;
        return;
    }
    oe->rot_rescale = re->rot_rescale;
    double ox, oy, oz;
    transform_point(re->rot_origin[0], re->rot_origin[1], re->rot_origin[2], kx, ky, &ox, &oy, &oz);
    oe->rot_origin[0] = ox; oe->rot_origin[1] = oy; oe->rot_origin[2] = oz;
    double av[3] = {0, 0, 0};
    av[re->rot_axis] = 1.0;
    double ax, ay, az;
    transform_dir(av[0], av[1], av[2], kx, ky, &ax, &ay, &az);
    double a[3] = {ax, ay, az};
    for (int i = 0; i < 3; i++) {
        if (a[i] != 0.0) { oe->rot_axis = i; oe->rot_angle = re->rot_angle * a[i]; break; }
    }
}

static void append_rotated_elements(const raw_element_t *raw, uint32_t raw_count, int kx, int ky,
                                    const texture_var_t *vars, uint32_t var_count,
                                    blockmodel_registry_t *reg,
                                    blockmodel_element_t *out, uint32_t *out_count, uint32_t out_cap) {
    for (uint32_t i = 0; i < raw_count && *out_count < out_cap; i++) {
        const raw_element_t *re = &raw[i];
        double ax, ay, az, bx, by, bz;
        transform_point(re->x0, re->y0, re->z0, kx, ky, &ax, &ay, &az);
        transform_point(re->x1, re->y1, re->z1, kx, ky, &bx, &by, &bz);
        blockmodel_element_t *oe = &out[(*out_count)++];
        oe->x0 = ax < bx ? ax : bx; oe->x1 = ax < bx ? bx : ax;
        oe->y0 = ay < by ? ay : by; oe->y1 = ay < by ? by : ay;
        oe->z0 = az < bz ? az : bz; oe->z1 = az < bz ? bz : az;
        compose_element_rotation(re, kx, ky, oe);
        memset(oe->faces, 0, sizeof(oe->faces));
        for (int fdir = 0; fdir < FACE_COUNT; fdir++) {
            const raw_face_t *rf = &re->faces[fdir];
            if (!rf->present) continue;
            block_face_dir_t d2 = rotate_face_x((block_face_dir_t)fdir, kx);
            block_face_dir_t d3 = rotate_face_y(d2, ky);
            blockmodel_shape_face_t *of = &oe->faces[d3];
            char literal[MAX_VAR_VALUE];
            memcpy(literal, rf->var_or_literal, sizeof(literal));
            if (bm_resolve_var_chain(vars, var_count, literal, sizeof(literal)) != 0) continue;
            of->texture = bm_load_texture_cached(reg, literal, strlen(literal));
            of->tint = rf->tint;
            of->uvrot = (rf->uvrot + induced_uvrot(fdir, kx, ky, (int)d3)) % 360;
            of->u0 = (float)rf->u0; of->v0 = (float)rf->v0; of->u1 = (float)rf->u1; of->v1 = (float)rf->v1;
        }
    }
}

/* ------------------------------------------------------------------- */
/* blockstate matching                                                  */
/* ------------------------------------------------------------------- */

static int find_prop(const blockmodel_property_t *props, size_t prop_count, const char *name, size_t name_len,
                     const char **out_val, size_t *out_val_len) {
    for (size_t i = 0; i < prop_count; i++) {
        if (props[i].name_len == name_len && memcmp(props[i].name, name, name_len) == 0) {
            *out_val = props[i].value;
            *out_val_len = props[i].value_len;
            return 1;
        }
    }
    return 0;
}

/* `want` may be "|"-alternated (redstone_wire.json: "side|up") - any
 * segment matching `have` counts as a match. */
static int prop_value_matches(const char *have, size_t have_len, const char *want, size_t want_len) {
    size_t start = 0;
    for (size_t i = 0; i <= want_len; i++) {
        if (i == want_len || want[i] == '|') {
            size_t seg_len = i - start;
            if (seg_len == have_len && memcmp(want + start, have, have_len) == 0) return 1;
            start = i + 1;
        }
    }
    return 0;
}

/* "facing=east,half=bottom,shape=straight" (or "" for a block with no
 * properties, blockstates' single always-matching variant key). */
int bm_variant_key_matches(const char *key, size_t key_len,
                           const blockmodel_property_t *props, size_t prop_count) {
    if (key_len == 0) return 1;
    size_t i = 0;
    while (i < key_len) {
        size_t seg_start = i;
        while (i < key_len && key[i] != ',') i++;
        size_t seg_len = i - seg_start;
        const char *eq = (const char *)memchr(key + seg_start, '=', seg_len);
        if (!eq) return 0;
        size_t nlen = (size_t)(eq - (key + seg_start));
        const char *val = eq + 1;
        size_t vlen = seg_len - nlen - 1;
        const char *have_val;
        size_t have_len;
        if (!find_prop(props, prop_count, key + seg_start, nlen, &have_val, &have_len)) return 0;
        if (have_len != vlen || memcmp(have_val, val, vlen) != 0) return 0;
        if (i < key_len) i++; /* skip comma */
    }
    return 1;
}

/* "when": an object's keys AND together (each may be "|"-alternated); a key
 * "OR" holds sub-objects of which any must match, "AND" sub-objects that
 * all must. No `when` always applies. */
static int when_matches(const json_pool_t *pool, const json_value_t *when,
                        const blockmodel_property_t *props, size_t prop_count) {
    if (!when) return 1;
    if (when->type != JSON_OBJECT) return 0;
    for (uint32_t i = 0; i < json_object_count(when); i++) {
        uint32_t klen;
        const char *k = json_object_key_at(pool, when, i, &klen);
        json_value_t *v = json_object_value_at(pool, when, i);
        if (klen == 2 && memcmp(k, "OR", 2) == 0) {
            if (v->type != JSON_ARRAY) return 0;
            int any = 0;
            for (uint32_t j = 0; j < json_array_count(v); j++) {
                if (when_matches(pool, json_array_at(pool, v, j), props, prop_count)) { any = 1; break; }
            }
            if (!any) return 0;
            continue;
        }
        if (klen == 3 && memcmp(k, "AND", 3) == 0) {
            if (v->type != JSON_ARRAY) return 0;
            for (uint32_t j = 0; j < json_array_count(v); j++) {
                if (!when_matches(pool, json_array_at(pool, v, j), props, prop_count)) return 0;
            }
            continue;
        }
        if (v->type != JSON_STRING) return 0;
        const char *have_val;
        size_t have_len;
        if (!find_prop(props, prop_count, k, klen, &have_val, &have_len)) return 0;
        if (!prop_value_matches(have_val, have_len, v->string.ptr, v->string.len)) return 0;
    }
    return 1;
}

/* `entry` is one {"model":...,"x":...,"y":...} or an array of weighted
 * alternatives; a static render takes index 0. */
static int extract_model_xy(const json_pool_t *pool, const json_value_t *entry,
                            char *model_ref, size_t *model_ref_len, size_t cap,
                            int *kx, int *ky) {
    const json_value_t *e = entry;
    if (e->type == JSON_ARRAY) {
        if (json_array_count(e) == 0) return -1;
        e = json_array_at(pool, e, 0);
    }
    if (e->type != JSON_OBJECT) return -1;
    json_value_t *model_val = json_object_getz(pool, e, "model");
    if (!model_val || model_val->type != JSON_STRING || model_val->string.len >= cap) return -1;
    memcpy(model_ref, model_val->string.ptr, model_val->string.len);
    *model_ref_len = model_val->string.len;
    model_ref[*model_ref_len] = '\0';
    json_value_t *xv = json_object_getz(pool, e, "x");
    json_value_t *yv = json_object_getz(pool, e, "y");
    int xdeg = (xv && xv->type == JSON_NUMBER) ? (int)xv->number : 0;
    int ydeg = (yv && yv->type == JSON_NUMBER) ? (int)yv->number : 0;
    *kx = ((xdeg / 90) % 4 + 4) % 4;
    *ky = ((ydeg / 90) % 4 + 4) % 4;
    return 0;
}

/* ------------------------------------------------------------------- */
/* the public entry point + shape cache                                 */
/* ------------------------------------------------------------------- */

static uint32_t shape_key_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static size_t build_shape_key(char *buf, size_t cap, const char *name, size_t name_len,
                              const blockmodel_property_t *props, size_t prop_count) {
    if (name_len > cap) return 0;
    memcpy(buf, name, name_len);
    size_t n = name_len;
    for (size_t i = 0; i < prop_count; i++) {
        size_t need = 1 + props[i].name_len + 1 + props[i].value_len;
        if (n + need > cap) return 0;
        buf[n++] = ';';
        memcpy(buf + n, props[i].name, props[i].name_len); n += props[i].name_len;
        buf[n++] = '=';
        memcpy(buf + n, props[i].value, props[i].value_len); n += props[i].value_len;
    }
    return n;
}

/* Applies state tint, takes a cache slot, computes has_uvrot/full_cube/
 * flat_equivalent and publishes. Shared by the blockstate and block-entity
 * paths. `slot_idx` is the empty slot the earlier probe found. */
static blockmodel_result_t finish_shape(blockmodel_registry_t *reg,
                                        const char *key, size_t key_len, uint32_t slot_idx,
                                        const char *block_name, size_t name_len,
                                        const blockmodel_property_t *props, size_t prop_count,
                                        blockmodel_element_t *accum, uint32_t accum_count, int any_rot,
                                        const blockmodel_shape_t **out_shape) {
    /* State-driven tint (redstone_wire) onto every tinted face, while the
     * props are at hand. Block entities set their own colours. */
    uint8_t state_rgb[3];
    if (bm_state_tint(block_name, name_len, props, prop_count, state_rgb)) {
        for (uint32_t i = 0; i < accum_count; i++) {
            for (int f = 0; f < FACE_COUNT; f++) {
                blockmodel_shape_face_t *sf = &accum[i].faces[f];
                if (sf->tint && !sf->has_tint_rgb) {
                    sf->has_tint_rgb = 1;
                    memcpy(sf->tint_rgb, state_rgb, 3);
                }
            }
        }
    }

    shape_cache_entry_t *slot = &reg->shapes[reg->shape_count];
    slot->key = (char *)malloc(key_len);
    if (!slot->key) return BLOCKMODEL_ERR_OOM;
    memcpy(slot->key, key, key_len);
    slot->key_len = key_len;
    if (accum_count > 0) {
        slot->shape.elements = (blockmodel_element_t *)malloc(accum_count * sizeof(blockmodel_element_t));
        if (!slot->shape.elements) { free(slot->key); return BLOCKMODEL_ERR_OOM; }
        memcpy(slot->shape.elements, accum, accum_count * sizeof(blockmodel_element_t));
    } else {
        slot->shape.elements = NULL;
    }
    slot->shape.element_count = accum_count;
    slot->shape.rotated = any_rot;
    slot->shape.has_uvrot = 0;
    for (uint32_t i = 0; i < accum_count && !slot->shape.has_uvrot; i++)
        for (int f = 0; f < FACE_COUNT; f++)
            if (accum[i].faces[f].uvrot) { slot->shape.has_uvrot = 1; break; }
    /* full_cube: every element spans 0..16 (with a float tolerance). An
     * empty shape isn't one. */
    int full = accum_count > 0;
    for (uint32_t i = 0; i < accum_count; i++) {
        const blockmodel_element_t *el = &slot->shape.elements[i];
        if (el->x0 > 0.01 || el->y0 > 0.01 || el->z0 > 0.01 ||
            el->x1 < 15.99 || el->y1 < 15.99 || el->z1 < 15.99) {
            full = 0;
            break;
        }
    }
    slot->shape.full_cube = full;
    slot->shape.flat_equivalent = full && !slot->shape.rotated && !slot->shape.has_uvrot;
    reg->shape_count++;
    reg->shape_index[slot_idx] = reg->shape_count; /* index + 1; slot_idx from the probe above */

    *out_shape = &slot->shape;
    return BLOCKMODEL_OK;
}

/* Read-only probe of the open-addressed shape_index - same shape as
 * texture_probe/faces_probe in registry.c/faces.c. */
static const blockmodel_shape_t *shape_probe(blockmodel_registry_t *reg, const char *key, size_t key_len,
                                             uint32_t *inout_slot) {
    uint32_t slot = *inout_slot;
    while (reg->shape_index[slot]) {
        shape_cache_entry_t *hit = &reg->shapes[reg->shape_index[slot] - 1];
        if (hit->key_len == key_len && memcmp(hit->key, key, key_len) == 0) return &hit->shape;
        slot = (slot + 1) & (SHAPE_INDEX_SLOTS - 1);
    }
    *inout_slot = slot;
    return NULL;
}

blockmodel_result_t blockmodel_resolve_shape(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                                             const blockmodel_property_t *props, size_t prop_count,
                                             const blockmodel_shape_t **out_shape) {
    char key[MAX_SHAPE_KEY];
    size_t key_len = build_shape_key(key, sizeof(key), block_name, name_len, props, prop_count);
    if (key_len == 0) return BLOCKMODEL_ERR_UNSUPPORTED;

    uint32_t slot0 = shape_key_hash(key, key_len) & (SHAPE_INDEX_SLOTS - 1);
    uint32_t shard = prism_get_shard_id();

    uint32_t slot_idx = slot0;
    percpu_rwlock_rdlock(&reg->lock, shard);
    const blockmodel_shape_t *hit = shape_probe(reg, key, key_len, &slot_idx);
    percpu_rwlock_rdunlock(&reg->lock, shard);
    if (hit) { *out_shape = hit; return BLOCKMODEL_OK; }

    /* Resolve outside the lock; the publish at the end takes the write lock
     * and re-probes first. */
    blockmodel_element_t accum[MAX_SHAPE_ELEMENTS];
    uint32_t accum_count = 0;
    int any_rot = 0; /* did any matched variant/part carry an x/y rotation? */
    blockmodel_result_t result = BLOCKMODEL_OK;

    /* Block entities: synthesized geometry, before the blockstate JSON
     * (which would give an empty shape). */
    if (bm_entity_shape(reg, block_name, name_len, props, prop_count,
                        accum, &accum_count, MAX_SHAPE_ELEMENTS, &any_rot)) {
        goto publish;
    }

    uint8_t *data;
    json_pool_t pool;
    json_value_t *root;
    blockmodel_result_t lr = bm_load_json(reg, "blockstates/", block_name, name_len, &data, &pool, &root);
    if (lr == BLOCKMODEL_ERR_UNSUPPORTED) return BLOCKMODEL_ERR_NOT_FOUND; /* over-long name: same as before */
    if (lr != BLOCKMODEL_OK) return lr;

    json_value_t *variants = json_object_getz(&pool, root, "variants");
    json_value_t *multipart = json_object_getz(&pool, root, "multipart");

    if (variants && variants->type == JSON_OBJECT && json_object_count(variants) > 0) {
        json_value_t *val = NULL;
        for (uint32_t i = 0; i < json_object_count(variants); i++) {
            uint32_t klen;
            const char *k = json_object_key_at(&pool, variants, i, &klen);
            if (bm_variant_key_matches(k, klen, props, prop_count)) {
                val = json_object_value_at(&pool, variants, i);
                break;
            }
        }
        /* No variant matched (a name-only probe): fall back to the first,
         * as faces.c does. */
        if (!val) val = json_object_value_at(&pool, variants, 0);
        char model_ref[MAX_PATH_LEN];
        size_t model_ref_len;
        int kx, ky;
        if (extract_model_xy(&pool, val, model_ref, &model_ref_len, sizeof(model_ref), &kx, &ky) == 0) {
            if (kx || ky) any_rot = 1;
            texture_var_t vars[MAX_TEXTURE_VARS];
            uint32_t var_count = 0;
            raw_element_t raw[MAX_ELEMENTS_PER_MODEL];
            uint32_t raw_count = 0;
            if (bm_resolve_model_chain_shape(reg, model_ref, model_ref_len, vars, &var_count, raw, &raw_count) == BLOCKMODEL_OK) {
                append_rotated_elements(raw, raw_count, kx, ky, vars, var_count, reg, accum, &accum_count, MAX_SHAPE_ELEMENTS);
            }
        }
    } else if (multipart && multipart->type == JSON_ARRAY) {
        for (uint32_t i = 0; i < json_array_count(multipart); i++) {
            json_value_t *part = json_array_at(&pool, multipart, i);
            if (part->type != JSON_OBJECT) continue;
            json_value_t *when = json_object_getz(&pool, part, "when");
            if (!when_matches(&pool, when, props, prop_count)) continue;
            json_value_t *apply = json_object_getz(&pool, part, "apply");
            if (!apply) continue;
            char model_ref[MAX_PATH_LEN];
            size_t model_ref_len;
            int kx, ky;
            if (extract_model_xy(&pool, apply, model_ref, &model_ref_len, sizeof(model_ref), &kx, &ky) != 0) continue;
            if (kx || ky) any_rot = 1;
            texture_var_t vars[MAX_TEXTURE_VARS];
            uint32_t var_count = 0;
            raw_element_t raw[MAX_ELEMENTS_PER_MODEL];
            uint32_t raw_count = 0;
            if (bm_resolve_model_chain_shape(reg, model_ref, model_ref_len, vars, &var_count, raw, &raw_count) == BLOCKMODEL_OK) {
                append_rotated_elements(raw, raw_count, kx, ky, vars, var_count, reg, accum, &accum_count, MAX_SHAPE_ELEMENTS);
            }
        }
    } else {
        result = BLOCKMODEL_ERR_UNSUPPORTED;
    }

    bm_free_json(data, &pool);

    if (result != BLOCKMODEL_OK) return result;

publish:
    percpu_rwlock_wrlock(&reg->lock);
    slot_idx = slot0;
    hit = shape_probe(reg, key, key_len, &slot_idx); /* re-check: another thread may have inserted */
    if (hit) {
        *out_shape = hit;
        percpu_rwlock_wrunlock(&reg->lock);
        return BLOCKMODEL_OK;
    }
    if (reg->shape_count >= BLOCKMODEL_MAX_SHAPES) {
        percpu_rwlock_wrunlock(&reg->lock);
        return BLOCKMODEL_ERR_OOM;
    }
    blockmodel_result_t fr = finish_shape(reg, key, key_len, slot_idx, block_name, name_len,
                                          props, prop_count, accum, accum_count, any_rot, out_shape);
    percpu_rwlock_wrunlock(&reg->lock);
    return fr;
}
