#include "tint.h"

#include "canvas.h"
#include "json.h"
#include "percpu_rwlock.h"
#include "zip_reader.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TINT_MAX_BIOMES 160
#define TINT_NAME_MAX 96

typedef struct {
    char name[TINT_NAME_MAX];
    uint16_t len;
    tint_biome_t c;
} tint_entry_t;

struct tint_ctx {
    zip_archive_t jar;
    int jar_ok;
    const canvas_t *cm_grass, *cm_foliage, *cm_dry; /* owned by the blockmodel registry */
    tint_entry_t v[TINT_MAX_BIOMES];
    uint32_t n;
    tint_biome_t fallback; /* plains (temp 0.8 / downfall 0.4) */
    percpu_rwlock_t lock;  /* guards v[]/n - see the header's threading contract */
};

/* colormap-less fallbacks: the plains corner of the vanilla colormaps */
static const uint8_t DEF_GRASS[3] = {0x91, 0xBD, 0x59};
static const uint8_t DEF_FOLIAGE[3] = {0x77, 0xAB, 0x2F};
static const uint8_t DEF_DRY[3] = {0xA0, 0x8A, 0x5A};

/* Vanilla colormap lookup: u = 1 - temperature, v = 1 - downfall *
 * temperature, over the 256x256 triangle. */
static void colormap_sample
    (
        const canvas_t *cm,
        double temp, double downfall,
        const uint8_t def[3], uint8_t out[3]
    )
{
    if (!cm || !cm->pixels || cm->width < 2 || cm->height < 2)
    {
        memcpy(out, def, 3);
        return;
    }

    temp     = fmax(0.0, fmin(temp    , 1.0));
    downfall = fmax(0.0, fmin(downfall, 1.0));

    double rain = downfall * temp;
    uint32_t ux = (uint32_t) ((1.0 - temp) * (cm->width  - 1) + 0.5);
    uint32_t uy = (uint32_t) ((1.0 - rain) * (cm->height - 1) + 0.5);

    const uint8_t *p = cm->pixels + (uy * cm->stride) + (ux * 4);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];

    return;
}

/* effects.*_color is "#rrggbb" in current data packs (older packs used a
 * plain number) - accept both. */
static int color_from_json
    (
        const json_pool_t *pool,
        const json_value_t *v,
        uint8_t out[3]
    )
{
    (void)pool;
    if (!v) return 0;

    uint32_t c;

    if (v->type == JSON_NUMBER)
    {
        c = (uint32_t)v->number;
        goto ok;
    }

    if (v->type == JSON_STRING && v->string.len == 7 && v->string.ptr[0] == '#')
    {
        char *endptr;
        c = (uint32_t)strtoul(&v->string.ptr[1], &endptr, 16);
        
        if (endptr == &v->string.ptr[7])
            goto ok;
    }

    return 0;

ok:;
    out[0] = (uint8_t) (c >> 16  );
    out[1] = (uint8_t) (c >> 8   );
    out[2] = (uint8_t) (c        );
    return 1;
}

static int str_is(const json_value_t *v, const char *lit)
{
    size_t n = strlen(lit);
    return v && v->type == JSON_STRING && v->string.len == n && memcmp(v->string.ptr, lit, n) == 0;
}


static int biome_parse_json
    (
        tint_ctx_t *t, const char *name, uint16_t len,
        double *temp, double *downfall,
        uint8_t ov[3][3], int has_ov[3],
        int *mod_swamp, int *mod_dark
    )
{
    if (!t->jar_ok || len <= 10 || memcmp(name, "minecraft:", 10) != 0 || len - 10 >= 100)
        return 0;

    char path[160];
    snprintf(path, sizeof(path), "data/minecraft/worldgen/biome/%.*s.json", (int)(len - 10), name + 10);
    
    const zip_entry_t *e = zip_findz(&t->jar, path);
    if (!e) return 0;

    uint8_t *buf = NULL;
    size_t blen = 0;
    if (zip_extract(&t->jar, e, &buf, &blen) != ZIP_OK) return 0;

    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root = NULL;

    if (json_parse(&pool, (const char *)buf, blen, &root) != JSON_OK || !root || root->type != JSON_OBJECT)
    {
        free(buf);
        json_pool_free(&pool);
        return 0;
    }

    const json_value_t *v;
    if ((v = json_object_getz(&pool, root, "temperature")) && v->type == JSON_NUMBER)
        *temp = v->number;

    if ((v = json_object_getz(&pool, root, "downfall")) && v->type == JSON_NUMBER)
        *downfall = v->number;

    const json_value_t *fx = json_object_getz(&pool, root, "effects");
    if (fx && fx->type == JSON_OBJECT)
    {
        has_ov[0] = color_from_json(&pool, json_object_getz(&pool, fx, "grass_color"), ov[0]);
        has_ov[1] = color_from_json(&pool, json_object_getz(&pool, fx, "foliage_color"), ov[1]);
        has_ov[2] = color_from_json(&pool, json_object_getz(&pool, fx, "dry_foliage_color"), ov[2]);
        
        const json_value_t *m = json_object_getz(&pool, fx, "grass_color_modifier");
        *mod_swamp = str_is(m, "swamp");
        *mod_dark  = str_is(m, "dark_forest");
    }

    json_pool_free(&pool);
    free(buf);
    return 1;
}


/* One biome's colours: climate and effects from its JSON, colormap
 * samples, then overrides (replace) and modifiers (on top), as the game
 * does. */
static void biome_compute(tint_ctx_t *t, const char *name, uint16_t len, tint_biome_t *out)
{
    double temp = 0.8, downfall = 0.4;
    uint8_t ov[3][3];
    int has_ov[3] = {0, 0, 0}; /* grass, foliage, dry */
    int mod_swamp = 0, mod_dark = 0;

    biome_parse_json(t, name, len, &temp, &downfall, ov, has_ov, &mod_swamp, &mod_dark);

    colormap_sample(t->cm_grass, temp, downfall, DEF_GRASS, out->grass);
    colormap_sample(t->cm_foliage, temp, downfall, DEF_FOLIAGE, out->foliage);
    colormap_sample(t->cm_dry, temp, downfall, DEF_DRY, out->dry);
    
    if (has_ov[0]) memcpy(out->grass, ov[0], 3);
    if (has_ov[1]) memcpy(out->foliage, ov[1], 3);
    if (has_ov[2]) memcpy(out->dry, ov[2], 3);
    
    if (mod_swamp) {
        out->grass[0] = 0x6A; out->grass[1] = 0x70; out->grass[2] = 0x39;
    } else if (mod_dark) {
        out->grass[0] = (uint8_t)(((out->grass[0] & 0xFE) + 0x28) >> 1);
        out->grass[1] = (uint8_t)(((out->grass[1] & 0xFE) + 0x34) >> 1);
        out->grass[2] = (uint8_t)(((out->grass[2] & 0xFE) + 0x0A) >> 1);
    }
}

tint_ctx_t *tint_create(const char *jar_path, blockmodel_registry_t *reg) {
    tint_ctx_t *t = (tint_ctx_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->jar_ok = jar_path && zip_open(jar_path, &t->jar) == ZIP_OK;
    t->cm_grass = blockmodel_texture(reg, "minecraft:colormap/grass", 24);
    t->cm_foliage = blockmodel_texture(reg, "minecraft:colormap/foliage", 26);
    t->cm_dry = blockmodel_texture(reg, "minecraft:colormap/dry_foliage", 30);
    /* fallback = plains climate through the same pipeline (no JSON read) */
    colormap_sample(t->cm_grass, 0.8, 0.4, DEF_GRASS, t->fallback.grass);
    colormap_sample(t->cm_foliage, 0.8, 0.4, DEF_FOLIAGE, t->fallback.foliage);
    colormap_sample(t->cm_dry, 0.8, 0.4, DEF_DRY, t->fallback.dry);
    return t;
}

int tint_set_shards(tint_ctx_t *t, uint32_t nshards) {
    return percpu_rwlock_init(&t->lock, nshards);
}

void tint_free(tint_ctx_t *t)
{
    if (!t) return;
    if (t->jar_ok) zip_close(&t->jar);
    percpu_rwlock_destroy(&t->lock);
    free(t);
}

static const tint_entry_t *biome_find(const tint_ctx_t *t, const char *name, uint16_t len)
{
    for (uint32_t i = 0; i < t->n; i++)
    {
        if (t->v[i].len == len && memcmp(t->v[i].name, name, len) == 0) return &t->v[i];
    }
    return NULL;
}

void tint_prepare(tint_ctx_t *t, const char *biome_name, uint16_t len)
{
    if (!t || !biome_name || len == 0 || len >= TINT_NAME_MAX) return;

    /* Fast path: already prepared; read lock on this thread's shard. */
    uint32_t shard = prism_get_shard_id();
    percpu_rwlock_rdlock(&t->lock, shard);
    int hit = biome_find(t, biome_name, len) != NULL;
    percpu_rwlock_rdunlock(&t->lock, shard);
    if (hit) return;

    /* Slow path, rare (at most TINT_MAX_BIOMES): write lock, then check
     * again. */
    percpu_rwlock_wrlock(&t->lock);
    if (!biome_find(t, biome_name, len) && t->n < TINT_MAX_BIOMES) {
        tint_entry_t *e = &t->v[t->n];
        memcpy(e->name, biome_name, len);
        e->name[len] = '\0';
        e->len = len;
        biome_compute(t, biome_name, len, &e->c);
        t->n++;
    } /* table full: degrade to fallback colors, same as before */
    percpu_rwlock_wrunlock(&t->lock);
}

const tint_biome_t *tint_biome_get(const tint_ctx_t *t, const char *biome_name, uint16_t len)
{
    static const tint_biome_t hard_fallback = {
        {0x91, 0xBD, 0x59}, {0x77, 0xAB, 0x2F}, {0xA0, 0x8A, 0x5A}
    };

    if (!t) return
        &hard_fallback;

    if (biome_name && len)
    {
        /* The lock is mutable state behind a const accessor. */
        percpu_rwlock_t *lock = (percpu_rwlock_t *)&t->lock;
        uint32_t shard = prism_get_shard_id();
        percpu_rwlock_rdlock(lock, shard);
        const tint_entry_t *e = biome_find(t, biome_name, len);
        percpu_rwlock_rdunlock(lock, shard);
        /* e points into t->v[], never reallocated/moved/removed once
         * written (see tint_prepare) - safe to read after unlocking. */
        if (e) return &e->c;
    }
    return &t->fallback;
}

static int name_eq(const char *name, uint16_t len, const char *lit)
{
    size_t n = strlen(lit);
    return n == len && memcmp(name, lit, len) == 0;
}

/* Blocks the game colours with its grass provider. "tintindex" only says
 * a face can be coloured; the colour comes from the block's registered
 * provider. Bamboo's leaf models are tinted too. Melon/pumpkin stems
 * really use an age gradient; grass is the stand-in. */
static int is_grass_tinted(const char *block, uint16_t len) {
    static const char *const grass[] = {
        "minecraft:grass_block", "minecraft:short_grass", "minecraft:fern",
        "minecraft:tall_grass", "minecraft:large_fern", "minecraft:potted_fern",
        "minecraft:sugar_cane", "minecraft:bamboo",
        "minecraft:melon_stem", "minecraft:pumpkin_stem",
        "minecraft:attached_melon_stem", "minecraft:attached_pumpkin_stem",
    };
    for (size_t i = 0; i < sizeof(grass) / sizeof(grass[0]); i++)
        if (name_eq(block, len, grass[i])) return 1;
    return 0;
}

tint_kind_t tint_classify(const char *block, uint16_t len, uint8_t fixed[3])
{
    /* Default is no tint: many blocks have a "tintindex" the game never
     * colours (stonecutter, cauldron fluids, pink_petals). */
    if (len >= 7 && memcmp(block + len - 7, "_leaves", 7) == 0) {
        /* the game's BlockColors: four leaves take the foliage colormap,
         * three a fixed color; the rest (cherry, pale_oak) keep their
         * texture's own color despite the tintindex in block/leaves */
        static const char *const foliage[] = {
            "minecraft:oak_leaves", "minecraft:jungle_leaves",
            "minecraft:acacia_leaves", "minecraft:dark_oak_leaves",
        };
        static const struct { const char *name; uint8_t rgb[3]; } leaves[] = {
            {"minecraft:birch_leaves", {0x80, 0xA7, 0x55}},
            {"minecraft:spruce_leaves", {0x61, 0x99, 0x61}},
            {"minecraft:mangrove_leaves", {0x92, 0xC6, 0x48}},
        };
        for (size_t i = 0; i < sizeof(foliage) / sizeof(foliage[0]); i++)
            if (name_eq(block, len, foliage[i])) return TINT_FOLIAGE;
        for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
            if (name_eq(block, len, leaves[i].name)) { memcpy(fixed, leaves[i].rgb, 3); return TINT_FIXED; }
        }
        return TINT_NONE;
    } else if (name_eq(block, len, "minecraft:vine")) {
        return TINT_FOLIAGE;
    } else if (name_eq(block, len, "minecraft:leaf_litter")) {
        return TINT_DRY;
    } else if (name_eq(block, len, "minecraft:lily_pad")) {
        /* fixed in game code, not climate-driven */
        fixed[0] = 0x20; fixed[1] = 0x80; fixed[2] = 0x30;
        return TINT_FIXED;
    } else if (is_grass_tinted(block, len)) {
        return TINT_GRASS;
    }
    return TINT_NONE;
}

void tint_resolve(const tint_biome_t *bio, tint_kind_t kind, const uint8_t fixed[3],
                  uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t white[3] = {0xFF, 0xFF, 0xFF};
    const uint8_t *c = white;
    switch (kind) {
        case TINT_GRASS:   c = bio->grass;   break;
        case TINT_FOLIAGE: c = bio->foliage; break;
        case TINT_DRY:     c = bio->dry;     break;
        case TINT_FIXED:   c = fixed;        break;
        case TINT_NONE:    break;
    }
    *r = c[0]; *g = c[1]; *b = c[2];
}

void tint_block_color
    (
        const tint_biome_t *bio,
        const char *block, uint16_t len,
        uint8_t *r, uint8_t *g, uint8_t *b
    )
{
    uint8_t fixed[3];
    tint_kind_t kind = tint_classify(block, len, fixed);
    tint_resolve(bio, kind, fixed, r, g, b);
}
