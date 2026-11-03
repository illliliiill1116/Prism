/*
 * blockmodel.h - resolves blocks to what Minecraft draws for them, from
 * the client jar's own data: blockstates pick a model, models resolve
 * texture variables through their "parent" chain, and textures supply the
 * pixels (via ../png). Built on ../zip and ../mcjson.
 *
 * Two levels: blockmodel_resolve/_state give six flat face textures;
 * blockmodel_resolve_shape gives a state's real element geometry.
 */
#ifndef PRISM_BLOCKMODEL_H
#define PRISM_BLOCKMODEL_H

#include "canvas.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum blockmodel_result {
    BLOCKMODEL_OK = 0,
    BLOCKMODEL_ERR_IO,           /* couldn't open the jar */
    BLOCKMODEL_ERR_NOT_FOUND,    /* no blockstates/<name>.json in the jar */
    BLOCKMODEL_ERR_UNSUPPORTED,  /* e.g. "multipart" blockstates (fences, walls, redstone wire, ...) */
    BLOCKMODEL_ERR_OOM,
} blockmodel_result_t;

typedef enum block_face_dir {
    FACE_UP = 0,
    FACE_DOWN,
    FACE_NORTH,
    FACE_SOUTH,
    FACE_EAST,
    FACE_WEST,
    FACE_COUNT,
} block_face_dir_t;

typedef struct block_faces {
    const canvas_t *texture[FACE_COUNT];    /* NULL if this face couldn't be resolved */
    int tint[FACE_COUNT];                   /* 1 if the model marks this face with a tintindex */
} block_faces_t;

typedef struct blockmodel_registry blockmodel_registry_t;

/* Opens `jar_path` (a Minecraft client jar) and initializes empty caches.
 * Nothing is read from the jar until blockmodel_resolve() asks for it. */
blockmodel_result_t blockmodel_open(const char *jar_path, blockmodel_registry_t **out_reg);
void blockmodel_close(blockmodel_registry_t *reg);

/* Resizes the cache lock to `nshards` reader shards (percpu_rwlock.h; one
 * after blockmodel_open). Call once, before any resolve and before other
 * threads use the registry. Returns 0 on OOM. */
int blockmodel_set_shards(blockmodel_registry_t *reg, uint32_t nshards);

/*
 * Resolves and caches per-face textures for a block name (length given
 * explicitly), using the first blockstate variant. The result is valid
 * until blockmodel_close().
 *
 * On BLOCKMODEL_OK a face's texture may still be NULL (dangling variable,
 * missing file); treat that face as unresolved, not the whole block.
 */
blockmodel_result_t blockmodel_resolve(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                                       const block_faces_t **out_faces);

/* Fetches and caches a texture by reference, e.g. "minecraft:block/vine".
 * NULL if the jar doesn't have it. Valid until blockmodel_close. */
const canvas_t *blockmodel_texture(blockmodel_registry_t *reg, const char *ref, size_t ref_len);

/*
 * blockmodel_resolve_shape() - a block state's real geometry: "variants"
 * property matching, "multipart" "when" evaluation (AND within a clause,
 * "OR"/"AND" arrays, "|" value alternation), whole-model x/y rotation in
 * 90-degree steps, and every element's box with its own per-face
 * texture, uv and tint.
 *
 * `props`/`prop_count` describe the state (the chunk palette's
 * "Properties"); properties the block doesn't have are ignored, and most
 * blocks pass none. The cache key is the name plus the props in the order
 * given, so pass them in a consistent order. Valid until blockmodel_close.
 *
 * Not implemented: "uvlock", and the default uv of partial elements that
 * omit "uv" (falls back to [0,0,16,16], correct for every full cube).
 * Per-element "rotation" is reported (rot_*) but not applied to the box.
 */
typedef struct blockmodel_property {
    const char *name; size_t name_len;
    const char *value; size_t value_len;
} blockmodel_property_t;

typedef struct blockmodel_shape_face {
    const canvas_t *texture; /* NULL = no face here (open air on this side) */
    int uvrot;               /* texture rotation on this face, degrees
                              * (0/90/180/270): the face's own "rotation"
                              * plus any quarter turn the model's x/y
                              * rotation induced */
    int tint;                /* 1 if the model marks this face with a tintindex */
    /* tintindex says which faces are coloured, not the colour. Biome
     * colours are the renderer's job (from `tint`); a block coloured by its
     * own state (redstone_wire's power) gets has_tint_rgb = 1 and the
     * finished tint_rgb here. */
    int has_tint_rgb;
    uint8_t tint_rgb[3];
    float u0, v0, u1, v1;    /* 16ths of the texture; u0>u1 or v0>v1 means
                              * flipped. Fractional in vanilla (.5 steps). */
} blockmodel_shape_face_t;

typedef struct blockmodel_element {
    double x0, y0, z0, x1, y1, z1; /* 16ths, block-local, after the model's
                                    * x/y rotation; per-element rotation is
                                    * NOT applied */
    blockmodel_shape_face_t faces[FACE_COUNT];
    /* Per-element "rotation", composed with the model's x/y rotation and
     * expressed in this frame. rot_angle 0 = none. Spin the box rot_angle
     * degrees about rot_axis (0=x 1=y 2=z) through rot_origin for the true
     * geometry. Thin elements rotated 45 about y (chains, lantern hangers)
     * have zero thickness unrotated. */
    int rot_axis;
    double rot_angle;      /* degrees */
    double rot_origin[3];  /* 16ths */
    int rot_rescale;       /* "rescale": after spinning, scale the two axes
                            * across rot_axis by 1/cos(angle) about the origin
                            * (cross plants span the block corner to corner) */
    /* Then, if has_xf, move it by the affine xf: p' = xf * (p, 1), in 16ths
     * (a head turned in 22.5 degree steps whose jaw has a turn of its
     * own, a statue's posed limbs). */
    int has_xf;
    double xf[3][4];
} blockmodel_element_t;

typedef struct blockmodel_shape {
    blockmodel_element_t *elements;
    uint32_t element_count; /* 0 for a block that resolves to no geometry at all */
    int rotated;            /* 1 if a whole-model x/y rotation was applied;
                             * a rotated full cube (furnace facing, horizontal
                             * log) differs from the flat path */
    int has_uvrot;          /* 1 if any face has a nonzero uvrot (piston_side),
                             * which the flat path can't draw */
    int full_cube;          /* 1 if every element spans the whole 0..16 box:
                             * the shape hides what's behind it, rotated or
                             * not. 0 for partial and empty shapes. */
    int flat_equivalent;    /* full_cube && !rotated && !has_uvrot: draws
                             * pixel-identically on the flat 6-face path */
} blockmodel_shape_t;

blockmodel_result_t blockmodel_resolve_shape(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                                             const blockmodel_property_t *props, size_t prop_count,
                                             const blockmodel_shape_t **out_shape);

/* A decorated pot's texture: the base (neck, top and bottom) and its four
 * sides in one image, laid out as its shape's faces expect. sides[] are
 * the back, left, right and front sides' textures, e.g.
 * "minecraft:entity/decorated_pot/angler_pottery_pattern", NULL each (or
 * sides NULL) for a plain one. The caller's to canvas_free; empty if the
 * jar lacks the pot's textures. */
canvas_t blockmodel_pot_texture(blockmodel_registry_t *reg, const char *const sides[4]);

/* A pot sherd item ("minecraft:angler_pottery_sherd") as its pattern's
 * texture for blockmodel_pot_texture: 1, or 0 for anything else (a brick
 * makes a plain side). */
int blockmodel_pot_pattern(const char *item, size_t len, char *out, size_t cap);

/* A dye's texture colour (DyeColor.getTextureDiffuseColor), by name with or
 * without "minecraft:", e.g. "light_blue": 1 and rgb set, or 0 if unknown. */
int blockmodel_dye_color(const char *name, size_t len, uint8_t rgb[3]);

/*
 * blockmodel_resolve with the "variants" entry matching `props` (first
 * variant when nothing matches or prop_count == 0). "multipart" stays
 * BLOCKMODEL_ERR_UNSUPPORTED. Not cached: writes into the caller's
 * `out_faces`, whose texture pointers stay owned by `reg`.
 */
blockmodel_result_t blockmodel_resolve_state(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                                             const blockmodel_property_t *props, size_t prop_count,
                                             block_faces_t *out_faces);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BLOCKMODEL_H */
