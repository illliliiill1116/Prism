/*
 * tint.h - biome tint colours for "tintindex" faces. A biome's
 * temperature/downfall samples the jar's colormaps (grass, foliage,
 * dry_foliage), overridden or modified by the biome's own JSON
 * (effects.grass_color, foliage_color, dry_foliage_color,
 * grass_color_modifier) - all read from the jar. The only rule in code is
 * which colour family a block uses (tint_block_color).
 *
 * tint_prepare and tint_biome_get are safe from any number of threads,
 * each on its own shard id; tint_prepare runs per column during the scan.
 * Size the shards with tint_set_shards for the largest thread count used.
 */
#ifndef PRISM_TINT_H
#define PRISM_TINT_H

#include "blockmodel.h"

#include <stdint.h>

/* One biome's resolved tint colors (RGB). */
typedef struct tint_biome_st {
    uint8_t grass[3];
    uint8_t foliage[3];
    uint8_t dry[3]; /* dry foliage (leaf litter) */
} tint_biome_t;

typedef struct tint_ctx tint_ctx_t;

/* Opens its own handle on the jar (biome JSON is under data/) and takes
 * the colormaps via `reg`. A missing jar or colormap degrades to
 * plains-like defaults; NULL only on OOM. */
tint_ctx_t *tint_create(const char *jar_path, blockmodel_registry_t *reg);
void tint_free(tint_ctx_t *t);

/* Resizes the lock to at least `nshards`. Call once, before concurrent
 * use. Returns 0 on OOM. */
int tint_set_shards(tint_ctx_t *t, uint32_t nshards);

/* Intern + compute this biome's colors (no-op if already present or the
 * table is full). Thread-safe - see the header comment. */
void tint_prepare(tint_ctx_t *t, const char *biome_name, uint16_t len);

/* The biome's colors; unknown/unprepared names (and t == NULL) return the
 * plains-like fallback - never NULL. Read-only, thread-safe. */
const tint_biome_t *tint_biome_get(const tint_ctx_t *t, const char *biome_name, uint16_t len);

/* The per-block colour family, split from the per-biome colour so the
 * visual cache can classify a state once and the draw resolve it per
 * column. tint_block_color is classify followed by resolve. */
typedef enum {
    TINT_NONE = 0,   /* no biome tint - the texture's own color (white multiply) */
    TINT_GRASS,      /* bio->grass */
    TINT_FOLIAGE,    /* bio->foliage */
    TINT_DRY,        /* bio->dry (leaf_litter) */
    TINT_FIXED,      /* a game-hardcoded color, carried in `fixed` */
} tint_kind_t;

/* `block`'s tint family; for TINT_FIXED also writes the hardcoded colour
 * to fixed[3]. Depends on the name only. */
tint_kind_t tint_classify(const char *block, uint16_t len, uint8_t fixed[3]);

/* Resolve a classified family + biome into the tint RGB. */
void tint_resolve(const tint_biome_t *bio, tint_kind_t kind, const uint8_t fixed[3],
                  uint8_t *r, uint8_t *g, uint8_t *b);

/* The colour a biome-tinted face of `block` takes in `bio`. State-driven
 * tints (redstone power) arrive as finished RGB on the shape face
 * (blockmodel_shape_face_t.has_tint_rgb) and never come here. */
void tint_block_color(const tint_biome_t *bio, const char *block, uint16_t len,
                      uint8_t *r, uint8_t *g, uint8_t *b);

#endif /* PRISM_TINT_H */
