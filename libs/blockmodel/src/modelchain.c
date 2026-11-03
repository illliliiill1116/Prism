/*
 * modelchain.c - the model "parent" chain walk. Both walkers merge
 * "textures" leaf-wins. bm_resolve_model_chain flattens to six face
 * textures; bm_resolve_model_chain_shape collects element cuboids, where
 * "elements" does not merge: the leaf-most model defining a non-empty
 * array wins.
 */
#include "internal.h"

#include <string.h>

/* Merges one model's "textures" into `vars`, leaf-wins. A texture may be
 * {"sprite": "...", ...} instead of a string; the reference is "sprite". */
static void collect_texture_vars(const json_pool_t *pool, const json_value_t *root,
                                 texture_var_t *vars, uint32_t *var_count) {
    json_value_t *textures = json_object_getz(pool, root, "textures");
    if (!textures || textures->type != JSON_OBJECT) return;
    for (uint32_t i = 0; i < json_object_count(textures); i++) {
        uint32_t klen;
        const char *k = json_object_key_at(pool, textures, i, &klen);
        int already = 0;
        for (uint32_t v = 0; v < *var_count; v++) {
            if (strlen(vars[v].name) == klen && memcmp(vars[v].name, k, klen) == 0) { already = 1; break; }
        }
        if (!already && *var_count < MAX_TEXTURE_VARS && klen < MAX_VAR_NAME) {
            json_value_t *val = json_object_value_at(pool, textures, i);
            if (val->type == JSON_OBJECT) val = json_object_getz(pool, val, "sprite");
            if (val && val->type == JSON_STRING && val->string.len < MAX_VAR_VALUE) {
                texture_var_t *slot = &vars[*var_count];
                memcpy(slot->name, k, klen);
                slot->name[klen] = '\0';
                memcpy(slot->value, val->string.ptr, val->string.len);
                slot->value[val->string.len] = '\0';
                (*var_count)++;
            }
        }
    }
}

/* Copies root's "parent" ref into next_ref; returns 1 if there is one. */
static int get_parent_ref(const json_pool_t *pool, const json_value_t *root,
                          char next_ref[MAX_PATH_LEN], size_t *next_ref_len) {
    json_value_t *parent = json_object_getz(pool, root, "parent");
    if (!parent || parent->type != JSON_STRING || parent->string.len >= MAX_PATH_LEN) return 0;
    *next_ref_len = parent->string.len;
    memcpy(next_ref, parent->string.ptr, *next_ref_len);
    next_ref[*next_ref_len] = '\0';
    return 1;
}

/* Default uv when a face omits "uv": the element's from/to projected onto
 * the face's texture axes. Vanilla partial elements always give "uv", so
 * in practice this only runs for full cubes, where it is [0,0,16,16]. */
static void default_uv(int dir, double x0, double y0, double z0, double x1, double y1, double z1,
                       double *u0, double *v0, double *u1, double *v1) {
    switch (dir) {
    case FACE_UP: case FACE_DOWN: *u0 = x0; *v0 = z0; *u1 = x1; *v1 = z1; break;
    case FACE_NORTH: case FACE_SOUTH: *u0 = x0; *v0 = y0; *u1 = x1; *v1 = y1; break;
    default: *u0 = z0; *v0 = y0; *u1 = z1; *v1 = y1; break; /* EAST/WEST */
    }
}

blockmodel_result_t bm_resolve_model_chain(blockmodel_registry_t *reg, const char *model_ref, size_t model_ref_len,
                                           texture_var_t *vars, uint32_t *var_count,
                                           face_state_t faces[FACE_COUNT]) {
    if (model_ref_len >= MAX_PATH_LEN) return BLOCKMODEL_ERR_UNSUPPORTED;
    char ref_buf[MAX_PATH_LEN];
    memcpy(ref_buf, model_ref, model_ref_len);
    ref_buf[model_ref_len] = '\0';
    size_t ref_len = model_ref_len;

    for (int depth = 0; depth < MAX_MODEL_CHAIN_DEPTH; depth++) {
        uint8_t *data;
        json_pool_t pool;
        json_value_t *root;
        blockmodel_result_t lr = bm_load_json(reg, "models/", ref_buf, ref_len, &data, &pool, &root);
        if (lr != BLOCKMODEL_OK) return lr;

        collect_texture_vars(&pool, root, vars, var_count);

        json_value_t *elements = json_object_getz(&pool, root, "elements");
        if (elements && elements->type == JSON_ARRAY) {
            for (uint32_t ei = 0; ei < json_array_count(elements); ei++) {
                json_value_t *elem = json_array_at(&pool, elements, ei);
                json_value_t *facesobj = json_object_getz(&pool, elem, "faces");
                if (!facesobj || facesobj->type != JSON_OBJECT) continue;
                for (int f = 0; f < FACE_COUNT; f++) {
                    if (faces[f].resolved) continue;
                    json_value_t *faceval = json_object_getz(&pool, facesobj, bm_face_names[f]);
                    if (!faceval || faceval->type != JSON_OBJECT) continue;
                    json_value_t *texval = json_object_getz(&pool, faceval, "texture");
                    if (texval && texval->type == JSON_STRING && texval->string.len < MAX_VAR_VALUE) {
                        memcpy(faces[f].var_or_literal, texval->string.ptr, texval->string.len);
                        faces[f].var_or_literal[texval->string.len] = '\0';
                        faces[f].tint = json_object_getz(&pool, faceval, "tintindex") != NULL;
                        faces[f].resolved = 1;
                    }
                }
            }
        }

        char next_ref[MAX_PATH_LEN];
        size_t next_ref_len = 0;
        int has_parent = get_parent_ref(&pool, root, next_ref, &next_ref_len);
        bm_free_json(data, &pool);

        if (!has_parent) break;
        memcpy(ref_buf, next_ref, next_ref_len + 1);
        ref_len = next_ref_len;
    }

    return BLOCKMODEL_OK;
}

blockmodel_result_t bm_resolve_model_chain_shape(blockmodel_registry_t *reg, const char *model_ref, size_t model_ref_len,
                                                 texture_var_t *vars, uint32_t *var_count,
                                                 raw_element_t *elems, uint32_t *elem_count) {
    if (model_ref_len >= MAX_PATH_LEN) return BLOCKMODEL_ERR_UNSUPPORTED;
    char ref_buf[MAX_PATH_LEN];
    memcpy(ref_buf, model_ref, model_ref_len);
    ref_buf[model_ref_len] = '\0';
    size_t ref_len = model_ref_len;
    *elem_count = 0;
    int have_elements = 0;

    for (int depth = 0; depth < MAX_MODEL_CHAIN_DEPTH; depth++) {
        uint8_t *data;
        json_pool_t pool;
        json_value_t *root;
        blockmodel_result_t lr = bm_load_json(reg, "models/", ref_buf, ref_len, &data, &pool, &root);
        if (lr != BLOCKMODEL_OK) return lr;

        collect_texture_vars(&pool, root, vars, var_count);

        json_value_t *elements = json_object_getz(&pool, root, "elements");
        if (!have_elements && elements && elements->type == JSON_ARRAY && json_array_count(elements) > 0) {
            uint32_t n = json_array_count(elements);
            if (n > MAX_ELEMENTS_PER_MODEL) n = MAX_ELEMENTS_PER_MODEL;
            for (uint32_t ei = 0; ei < n; ei++) {
                json_value_t *elem = json_array_at(&pool, elements, ei);
                raw_element_t *re = &elems[ei];
                memset(re, 0, sizeof(*re));
                double f[3] = {0, 0, 0}, t[3] = {16, 16, 16};
                json_value_t *from = json_object_getz(&pool, elem, "from");
                json_value_t *to = json_object_getz(&pool, elem, "to");
                if (from && from->type == JSON_ARRAY && json_array_count(from) == 3) {
                    for (int c = 0; c < 3; c++) {
                        json_value_t *cv = json_array_at(&pool, from, c);
                        if (cv->type == JSON_NUMBER) f[c] = cv->number;
                    }
                }
                if (to && to->type == JSON_ARRAY && json_array_count(to) == 3) {
                    for (int c = 0; c < 3; c++) {
                        json_value_t *cv = json_array_at(&pool, to, c);
                        if (cv->type == JSON_NUMBER) t[c] = cv->number;
                    }
                }
                re->x0 = f[0]; re->y0 = f[1]; re->z0 = f[2];
                re->x1 = t[0]; re->y1 = t[1]; re->z1 = t[2];

                /* per-element "rotation" (origin/axis/angle); default
                 * origin is the block center [8,8,8] when omitted */
                re->rot_origin[0] = re->rot_origin[1] = re->rot_origin[2] = 8.0;
                json_value_t *rot = json_object_getz(&pool, elem, "rotation");
                if (rot && rot->type == JSON_OBJECT) {
                    json_value_t *ax = json_object_getz(&pool, rot, "axis");
                    json_value_t *ang = json_object_getz(&pool, rot, "angle");
                    json_value_t *org = json_object_getz(&pool, rot, "origin");
                    if (ax && ax->type == JSON_STRING && ax->string.len == 1 &&
                        ang && ang->type == JSON_NUMBER && ang->number != 0.0) {
                        char a = ax->string.ptr[0];
                        re->rot_axis = (a == 'x') ? 0 : (a == 'y') ? 1 : (a == 'z') ? 2 : -1;
                        if (re->rot_axis >= 0) {
                            re->rot_angle = ang->number;
                            json_value_t *rs = json_object_getz(&pool, rot, "rescale");
                            re->rot_rescale = rs && rs->type == JSON_TRUE;
                            if (org && org->type == JSON_ARRAY && json_array_count(org) == 3) {
                                for (int c = 0; c < 3; c++) {
                                    json_value_t *cv = json_array_at(&pool, org, c);
                                    if (cv->type == JSON_NUMBER) re->rot_origin[c] = cv->number;
                                }
                            }
                        }
                    }
                }

                json_value_t *facesobj = json_object_getz(&pool, elem, "faces");
                if (facesobj && facesobj->type == JSON_OBJECT) {
                    for (int fdir = 0; fdir < FACE_COUNT; fdir++) {
                        json_value_t *faceval = json_object_getz(&pool, facesobj, bm_face_names[fdir]);
                        if (!faceval || faceval->type != JSON_OBJECT) continue;
                        json_value_t *texval = json_object_getz(&pool, faceval, "texture");
                        if (!texval || texval->type != JSON_STRING || texval->string.len >= MAX_VAR_VALUE) continue;
                        raw_face_t *rf = &re->faces[fdir];
                        memcpy(rf->var_or_literal, texval->string.ptr, texval->string.len);
                        rf->var_or_literal[texval->string.len] = '\0';
                        rf->tint = json_object_getz(&pool, faceval, "tintindex") != NULL;
                        /* Per-face texture "rotation" (clockwise quarter
                         * turns), used by directional textures (logs,
                         * piston_side, doors). */
                        rf->uvrot = 0;
                        json_value_t *frot = json_object_getz(&pool, faceval, "rotation");
                        if (frot && frot->type == JSON_NUMBER) {
                            int a = ((int)frot->number % 360 + 360) % 360;
                            rf->uvrot = a - (a % 90); /* snap to a quarter turn */
                        }
                        json_value_t *uv = json_object_getz(&pool, faceval, "uv");
                        if (uv && uv->type == JSON_ARRAY && json_array_count(uv) == 4) {
                            double u[4];
                            for (int c = 0; c < 4; c++) {
                                json_value_t *cv = json_array_at(&pool, uv, c);
                                u[c] = (cv->type == JSON_NUMBER) ? cv->number : 0;
                            }
                            rf->u0 = u[0]; rf->v0 = u[1]; rf->u1 = u[2]; rf->v1 = u[3];
                        } else {
                            default_uv(fdir, re->x0, re->y0, re->z0, re->x1, re->y1, re->z1,
                                      &rf->u0, &rf->v0, &rf->u1, &rf->v1);
                        }
                        rf->present = 1;
                    }
                }
            }
            *elem_count = n;
            have_elements = 1;
        }

        char next_ref[MAX_PATH_LEN];
        size_t next_ref_len = 0;
        int has_parent = get_parent_ref(&pool, root, next_ref, &next_ref_len);
        bm_free_json(data, &pool);

        if (!has_parent) break;
        memcpy(ref_buf, next_ref, next_ref_len + 1);
        ref_len = next_ref_len;
    }

    return BLOCKMODEL_OK;
}
