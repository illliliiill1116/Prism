/*
 * internal.h - private to the blockmodel sources:
 *
 *   registry.c      open/close, cache storage, jar asset loading,
 *                   texture-variable chains
 *   modelchain.c    the "parent" chain walk (flat faces, raw elements)
 *   shape.c         blockmodel_resolve_shape and its cache
 *   faces.c         the flat six-face resolvers
 *   entity_shapes.c block-entity geometry
 *   statetint.c     state-driven tint colours
 */
#ifndef PRISM_BLOCKMODEL_INTERNAL_H
#define PRISM_BLOCKMODEL_INTERNAL_H

#include "blockmodel.h"
#include "json.h"
#include "percpu_rwlock.h"
#include "zip_reader.h"

#define MAX_TEXTURE_VARS 32
#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 160
#define MAX_PATH_LEN 256
#define MAX_MODEL_CHAIN_DEPTH 16

/* Fixed-capacity caches: returned pointers must stay valid as more is
 * resolved, so the arrays are allocated at full size and never grow.
 * Bounded by the number of distinct vanilla blocks and textures. */
#define BLOCKMODEL_MAX_TEXTURES 4096
#define BLOCKMODEL_MAX_BLOCKS 4096

/* The shape cache is per state; the debug world alone has ~32k states,
 * and running out degrades blocks to flat cubes silently. Indexed by a 2x
 * open-addressed hash. */
#define BLOCKMODEL_MAX_SHAPES 65536
#define SHAPE_INDEX_SLOTS (BLOCKMODEL_MAX_SHAPES * 2) /* power of two, <=50% load */

/* Hash indices for the texture/faces caches, so the write lock's hold
 * time doesn't grow with cache size. */
#define TEXTURE_INDEX_SLOTS (BLOCKMODEL_MAX_TEXTURES * 2)
#define FACES_INDEX_SLOTS (BLOCKMODEL_MAX_BLOCKS * 2)

typedef struct {
    char *key;
    size_t key_len;
    canvas_t image;
} texture_cache_entry_t;

typedef struct {
    char *key;
    size_t key_len;
    block_faces_t faces;
} faces_cache_entry_t;

typedef struct {
    char *key;
    size_t key_len;
    blockmodel_shape_t shape;
} shape_cache_entry_t;

struct blockmodel_registry {
    zip_archive_t jar;
    texture_cache_entry_t *textures;
    uint32_t texture_count;
    uint32_t *texture_index; /* TEXTURE_INDEX_SLOTS entries, value = textures[] index + 1, 0 = empty */
    faces_cache_entry_t *faces_cache;
    uint32_t faces_count;
    uint32_t *faces_index; /* FACES_INDEX_SLOTS entries, value = faces_cache[] index + 1, 0 = empty */
    shape_cache_entry_t *shapes;
    uint32_t shape_count;
    uint32_t *shape_index; /* SHAPE_INDEX_SLOTS entries, value = shapes[] index + 1, 0 = empty */
    /* One lock for all three caches: writes are rare and never held across
     * zip/JSON/PNG work. */
    percpu_rwlock_t lock;
};

/* "up"/"down"/"north"/... indexed by block_face_dir_t (registry.c) */
extern const char *const bm_face_names[FACE_COUNT];

typedef struct {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
} texture_var_t;

typedef struct {
    char var_or_literal[MAX_VAR_VALUE];
    int tint;
    int resolved;
} face_state_t;

typedef struct {
    int present;
    char var_or_literal[MAX_VAR_VALUE];
    int tint;
    int uvrot; /* per-face texture rotation, degrees (0/90/180/270) */
    double u0, v0, u1, v1;
} raw_face_t;

typedef struct {
    double x0, y0, z0, x1, y1, z1;
    raw_face_t faces[FACE_COUNT];
    /* per-element "rotation" (model space, applied before the whole-model
     * x/y rotation). rot_angle == 0 means none. rot_axis: 0=x 1=y 2=z. */
    int rot_axis;
    double rot_angle;       /* degrees */
    double rot_origin[3];   /* 16ths */
    int rot_rescale;        /* "rescale": stretch to span the block again */
} raw_element_t;

#define MAX_ELEMENTS_PER_MODEL 32
#define MAX_SHAPE_ELEMENTS 128
#define MAX_SHAPE_KEY 512

/* ---- registry.c ---------------------------------------------------- */

/* "minecraft:block/oak_planks" -> "assets/minecraft/<category><local><ext>" */
int bm_build_asset_path(char *buf, size_t buf_cap, const char *category,
                        const char *ref, size_t ref_len, const char *ext);

/* Extracts and parses one JSON asset. On success the caller frees all
 * three with bm_free_json; strings point into *out_data, which must outlive
 * the pool. */
blockmodel_result_t bm_load_json(blockmodel_registry_t *reg, const char *category,
                                 const char *ref, size_t ref_len,
                                 uint8_t **out_data, json_pool_t *out_pool, json_value_t **out_root);
void bm_free_json(uint8_t *data, json_pool_t *pool);

const canvas_t *bm_load_texture_cached(blockmodel_registry_t *reg, const char *ref, size_t ref_len);
/* The texture cache by key (a texture's path in the jar, or a made-up key
 * for one built here): get probes it; put adds *img (the cache takes it
 * over) and returns the cached copy, or the one already there under key
 * (freeing *img), or NULL when full. */
const canvas_t *bm_texture_get(blockmodel_registry_t *reg, const char *key, size_t key_len);
const canvas_t *bm_texture_put(blockmodel_registry_t *reg, const char *key, size_t key_len, canvas_t *img);

/* FNV-1a over a byte key, for the cache indices. */
uint32_t bm_key_hash(const char *s, size_t n);

/* Follows "#varname" indirections through `vars` to a literal texture
 * reference, written back into `inout`. -1 on a dangling reference or a
 * chain long enough to be a cycle. */
int bm_resolve_var_chain(const texture_var_t *vars, uint32_t var_count, char *inout, size_t inout_cap);

/* ---- modelchain.c --------------------------------------------------- */

/* Walk "parent" links merging "textures" leaf-wins and flattening face
 * textures to the 6 canonical directions (legacy blockmodel_resolve). */
blockmodel_result_t bm_resolve_model_chain(blockmodel_registry_t *reg, const char *model_ref, size_t model_ref_len,
                                           texture_var_t *vars, uint32_t *var_count,
                                           face_state_t faces[FACE_COUNT]);

/* Same walk, but collects raw (unrotated) element geometry - the
 * leaf-most non-empty "elements" array wins whole, not merged. */
blockmodel_result_t bm_resolve_model_chain_shape(blockmodel_registry_t *reg, const char *model_ref, size_t model_ref_len,
                                                 texture_var_t *vars, uint32_t *var_count,
                                                 raw_element_t *elems, uint32_t *elem_count);

/* ---- shape.c (shared with faces.c) --------------------------------- */

/* Does a "variants" key ("facing=north,half=lower", or "" for the
 * always-matching key) match these props? */
int bm_variant_key_matches(const char *key, size_t key_len,
                           const blockmodel_property_t *props, size_t prop_count);

/* ---- entity_shapes.c ------------------------------------------------ */

/* Block-entity geometry (entity_shapes.c). Returns 1 and fills
 * accum[0..*accum_count) (at most cap) when `block_name` is handled, 0
 * otherwise. *any_rot = 1 if a facing rotation was applied. */
int bm_entity_shape(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                    const blockmodel_property_t *props, size_t prop_count,
                    blockmodel_element_t *accum, uint32_t *accum_count, uint32_t cap,
                    int *any_rot);

/* ---- statetint.c ---------------------------------------------------- */

/* If this block colours its tinted faces from its own state, writes the
 * colour to `out_rgb` and returns 1; 0 for biome-driven blocks. */
int bm_state_tint(const char *block_name, size_t name_len,
                  const blockmodel_property_t *props, size_t prop_count,
                  uint8_t out_rgb[3]);

#endif /* PRISM_BLOCKMODEL_INTERNAL_H */
