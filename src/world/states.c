/* Block states: what each palette entry of the world is to the mesher and
 * the light (its shape, fluid, culling, occlusion, emission, plant-ness),
 * interned once per distinct state, from the client jar's models and tags. */
#include "prism.h"

state_t *states;

uint32_t nstates, cap_states;

uint32_t *htab; /* open addressing: state index + 1, 0 = empty */

uint32_t hcap = 1u << 15;

uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static int name_is(const char *name, const char *want) { return strcmp(name, want) == 0; }

static int name_has(const char *name, const char *part) { return strstr(name, part) != NULL; }

blockmodel_registry_t *reg;

const canvas_t *tex_water, *tex_lava, *tex_white;

static double shape_volume(const blockmodel_shape_t *sh) {
    double v = 0;
    for (uint32_t i = 0; i < sh->element_count; i++) {
        const blockmodel_element_t *e = &sh->elements[i];
        if (e->rot_angle != 0 || e->has_xf) continue;
        v += (e->x1 - e->x0) * (e->y1 - e->y0) * (e->z1 - e->z0);
    }
    return v / 4096.0;
}

/* Block light a state gives off. Like the plant offset, this is in the
 * game's code (Blocks.java), not in the jar's data. */
static int prop_int(const char *key, const char *prop) {
    const char *p = strstr(key, prop);
    return p ? atoi(p + strlen(prop)) : 0;
}

zip_archive_t jarzip;

/* the jar's JSON at path, parsed into pool (its strings point into *buf);
 * NULL if missing */
static json_value_t *jar_json(const char *path, json_pool_t *pool, uint8_t **buf) {
    const zip_entry_t *e = zip_findz(&jarzip, path);
    size_t len = 0;
    json_value_t *root = NULL;
    *buf = NULL;
    json_pool_init(pool);
    if (!e || zip_extract(&jarzip, e, buf, &len) != ZIP_OK) return NULL;
    return json_parse(pool, (const char *)*buf, len, &root) == JSON_OK ? root : NULL;
}

/* 1 if block tag `tag` (its own tags followed) lists minecraft:<bare> */
static int tag_has(const char *tag, const char *bare, int depth) {
    char path[512];
    snprintf(path, sizeof path, "data/minecraft/tags/block/%s.json", tag);
    json_pool_t pool;
    uint8_t *buf;
    json_value_t *root = jar_json(path, &pool, &buf), *v = root ? json_object_getz(&pool, root, "values") : NULL;
    int hit = 0;
    for (uint32_t i = 0; v && v->type == JSON_ARRAY && i < json_array_count(v) && !hit; i++) {
        const json_value_t *x = json_array_at(&pool, v, i);
        if (x->type != JSON_STRING || x->string.len < 11) continue;
        const char *str = x->string.ptr;
        size_t n = x->string.len;
        if (str[0] == '#' && depth < 4 && n > 11 && !strncmp(str, "#minecraft:", 11)) {
            char sub[128];
            snprintf(sub, sizeof sub, "%.*s", (int)(n - 11), str + 11);
            hit = tag_has(sub, bare, depth + 1);
        } else if (!strncmp(str, "minecraft:", 10) && n - 10 == strlen(bare) && !memcmp(str + 10, bare, n - 10))
            hit = 1;
    }
    json_pool_free(&pool);
    free(buf);
    return hit;
}

/* The model a block's first state (or part) draws and its parents: 1 if
 * one is a plant's - a cross, a crop, a stem, a flower bed. */
static int plant_model(const char *bare) {
    char path[512];
    snprintf(path, sizeof path, "assets/minecraft/blockstates/%s.json", bare);
    json_pool_t pool;
    uint8_t *buf;
    json_value_t *root = jar_json(path, &pool, &buf), *m = NULL;
    char model[128] = "";
    if (root) {
        json_value_t *vs = json_object_getz(&pool, root, "variants"), *mp = json_object_getz(&pool, root, "multipart");
        if (vs && vs->type == JSON_OBJECT && json_object_count(vs)) m = json_object_value_at(&pool, vs, 0);
        else if (mp && mp->type == JSON_ARRAY && json_array_count(mp)) m = json_object_getz(&pool, json_array_at(&pool, mp, 0), "apply");
        if (m && m->type == JSON_ARRAY && json_array_count(m)) m = json_array_at(&pool, m, 0);
        m = m && m->type == JSON_OBJECT ? json_object_getz(&pool, m, "model") : NULL;
        if (m && m->type == JSON_STRING) snprintf(model, sizeof model, "%.*s", (int)m->string.len, m->string.ptr);
    }
    json_pool_free(&pool);
    free(buf);
    for (int depth = 0; depth < 8 && model[0]; depth++) {
        const char *b = strncmp(model, "minecraft:", 10) == 0 ? model + 10 : model;
        if (name_is(b, "block/cross") || name_is(b, "block/tinted_cross") || name_is(b, "block/crop") ||
            !strncmp(b, "block/stem_", 11) || !strncmp(b, "block/flowerbed", 15))
            return 1;
        snprintf(path, sizeof path, "assets/minecraft/models/%s.json", b);
        root = jar_json(path, &pool, &buf);
        json_value_t *p = root ? json_object_getz(&pool, root, "parent") : NULL;
        if (p && p->type == JSON_STRING) snprintf(model, sizeof model, "%.*s", (int)p->string.len, p->string.ptr);
        else model[0] = 0;
        json_pool_free(&pool);
        free(buf);
    }
    return 0;
}

/* 1 if the block drops itself only to shears (grass, vines, small
 * dripleaf), as the game's loot table says */
static int loot_shears(const char *bare) {
    char path[512];
    snprintf(path, sizeof path, "data/minecraft/loot_table/blocks/%s.json", bare);
    const zip_entry_t *e = zip_findz(&jarzip, path);
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!e || zip_extract(&jarzip, e, &buf, &len) != ZIP_OK) return 0;
    static const char want[] = "\"minecraft:shears\"";
    int hit = 0;
    for (size_t i = 0; i + sizeof want - 1 <= len && !hit; i++) hit = !memcmp(buf + i, want, sizeof want - 1);
    free(buf);
    return hit;
}

/* A plant, by the game's data rather than a list of ours: its model is a
 * plant's (plant_model), a tag of plants names it, or it drops only to
 * shears. Not a ladder, a rail or dripstone, which are thin cut-outs too. */
static int is_plant(const char *bare) {
    static const char *const tags[] = { "replaceable_by_trees", "flowers", "crops", "saplings", "bee_growables" };
    if (plant_model(bare) || loot_shears(bare)) return 1;
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++)
        if (tag_has(tags[i], bare, 0)) return 1;
    return 0;
}

static int emission(const char *bare, const char *key) {
    static const struct { const char *name; uint8_t level; } E[] = {
        { "glowstone", 15 }, { "sea_lantern", 15 }, { "jack_o_lantern", 15 }, { "lantern", 15 },
        { "beacon", 15 }, { "conduit", 15 }, { "shroomlight", 15 }, { "lava", 15 }, { "fire", 15 },
        { "campfire", 15 }, { "redstone_lamp", 15 }, { "end_gateway", 15 }, { "end_portal", 15 },
        { "ochre_froglight", 15 }, { "verdant_froglight", 15 }, { "pearlescent_froglight", 15 },
        { "copper_bulb", 15 }, { "exposed_copper_bulb", 12 }, { "weathered_copper_bulb", 8 },
        { "oxidized_copper_bulb", 4 }, { "torch", 14 }, { "wall_torch", 14 }, { "end_rod", 14 },
        { "furnace", 13 }, { "blast_furnace", 13 }, { "smoker", 13 }, { "nether_portal", 11 },
        { "soul_torch", 10 }, { "soul_wall_torch", 10 }, { "soul_lantern", 10 }, { "soul_fire", 10 },
        { "soul_campfire", 10 }, { "crying_obsidian", 10 }, { "redstone_ore", 9 },
        { "deepslate_redstone_ore", 9 }, { "enchanting_table", 7 }, { "ender_chest", 7 },
        { "glow_lichen", 7 }, { "redstone_torch", 7 }, { "redstone_wall_torch", 7 },
        { "sculk_catalyst", 6 }, { "amethyst_cluster", 5 }, { "large_amethyst_bud", 4 },
        { "magma_block", 3 }, { "firefly_bush", 2 }, { "medium_amethyst_bud", 2 },
        { "small_amethyst_bud", 1 }, { "brewing_stand", 1 }, { "brown_mushroom", 1 },
        { "dragon_egg", 1 }, { "end_portal_frame", 1 }, { "sculk_sensor", 1 },
        { "calibrated_sculk_sensor", 1 },
    };
    if (strstr(key, "lit=false")) return 0;
    if (strncmp(bare, "waxed_", 6) == 0) bare += 6;
    size_t n = strlen(bare);
    if (name_is(bare, "light")) return prop_int(key, "level=");
    if (n >= 6 && strcmp(bare + n - 6, "candle") == 0) return 3 * prop_int(key, "candles=");
    if (n >= 11 && strcmp(bare + n - 11, "candle_cake") == 0) return 3;
    if (name_is(bare, "cave_vines") || name_is(bare, "cave_vines_plant")) return strstr(key, "berries=true") ? 14 : 0;
    if (name_is(bare, "sea_pickle")) return strstr(key, "waterlogged=true") ? 3 + 3 * prop_int(key, "pickles=") : 0;
    if (name_is(bare, "respawn_anchor")) { static const uint8_t c[5] = { 0, 3, 7, 11, 15 }; int k = prop_int(key, "charges="); return c[k < 0 ? 0 : k > 4 ? 4 : k]; }
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
        if (name_is(bare, E[i].name)) return E[i].level;
    return 0;
}

static uint32_t intern_named(const nbt_pool_t *pool, const char *name, uint16_t nlen, const nbt_tag_t *props);

uint32_t intern_state(const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t pi) {
    const char *name; uint16_t nlen;
    if (chunk_palette_entry_name(pool, palette, pi, &name, &nlen) != CHUNK_OK) return 0;
    const nbt_tag_t *props = NULL;
    chunk_palette_entry_properties(pool, palette, pi, &props);
    return intern_named(pool, name, nlen, props);
}

uint32_t intern_block_state(const nbt_pool_t *pool, const nbt_tag_t *bs) {
    const nbt_tag_t *n = bs && bs->type == NBT_COMPOUND ? nbt_compound_getz(pool, bs, "Name") : NULL;
    if (!n || n->type != NBT_STRING || n->string.len > 0xffff) return 0;
    const nbt_tag_t *props = nbt_compound_getz(pool, bs, "Properties");
    return intern_named(pool, n->string.data, (uint16_t)n->string.len, props && props->type == NBT_COMPOUND ? props : NULL);
}

static uint32_t intern_named(const nbt_pool_t *pool, const char *name, uint16_t nlen, const nbt_tag_t *props) {
    char key[512];
    size_t kl = 0;
    if (nlen >= sizeof key - 1) return 0;
    memcpy(key, name, nlen); kl = nlen;
    blockmodel_property_t bp[32];
    size_t nbp = 0;
    if (props) {
        for (uint32_t i = 0; i < props->child_count && nbp < 32; i++) {
            const nbt_tag_t *c = nbt_child_at(pool, props, i);
            if (c->type != NBT_STRING || !c->name) continue;
            if (kl + 3 + c->name_len + (size_t)c->string.len >= sizeof key) break;
            key[kl++] = nbp ? ',' : '|';
            memcpy(key + kl, c->name, c->name_len); kl += c->name_len;
            key[kl++] = '=';
            memcpy(key + kl, c->string.data, (size_t)c->string.len); kl += (size_t)c->string.len;
            bp[nbp++] = (blockmodel_property_t){ c->name, c->name_len, c->string.data, (size_t)c->string.len };
        }
    }
    key[kl] = 0;

    uint32_t h = fnv(key, kl) & (hcap - 1);
    for (;;) {
        uint32_t v = htab[h];
        if (!v) break;
        if (strcmp(states[v - 1].key, key) == 0) return v - 1;
        h = (h + 1) & (hcap - 1);
    }
    if (nstates == cap_states) {
        cap_states *= 2;
        states = realloc(states, cap_states * sizeof *states);
    }
    state_t *s = &states[nstates];
    memset(s, 0, sizeof *s);
    s->key = strdup(key);
    s->name_hash = fnv(name, nlen);
    char nm[256];
    snprintf(nm, sizeof nm, "%.*s", (int)nlen, name);
    const char *bare = strncmp(nm, "minecraft:", 10) == 0 ? nm + 10 : nm;

    if (name_is(bare, "air") || name_is(bare, "cave_air") || name_is(bare, "void_air")) {
        s->air = 1;
    } else {
        int waterlogged = strstr(key, "waterlogged=true") != NULL;
        if (name_is(bare, "water") || name_is(bare, "lava")) {
            s->fluid = name_is(bare, "water") ? FLUID_WATER : FLUID_LAVA;
            s->fluid_block = 1;
            int level = 0;
            const char *lv = strstr(key, "level=");
            if (lv) level = atoi(lv + 6);
            int full = s->fluid == FLUID_WATER ? 14 : 16;
            int hh = (level <= 0 || level >= 8) ? full : (8 - level) * full / 8;
            s->fluid_h = (uint8_t)(hh < 2 ? 2 : hh);
        } else if (waterlogged || name_is(bare, "bubble_column") || name_is(bare, "kelp") ||
                   name_is(bare, "kelp_plant") || name_is(bare, "seagrass") || name_is(bare, "tall_seagrass")) {
            s->fluid = FLUID_WATER;
            s->fluid_h = 14;
        }
        if (!s->fluid_block) {
            const blockmodel_shape_t *sh = NULL;
            if (blockmodel_resolve_shape(reg, name, nlen, bp, nbp, &sh) == BLOCKMODEL_OK) s->shape = sh;
        }
        if (s->shape && s->shape->element_count) {
            int opaque = 1;
            for (uint32_t i = 0; i < s->shape->element_count; i++)
                for (int f = 0; f < FACE_COUNT; f++) {
                    const canvas_t *t = s->shape->elements[i].faces[f].texture;
                    if (t && !tex_opaque(t)) opaque = 0;
                }
            /* A full cube hides what is behind it when every side has an
             * unrotated element face covering it whole with an opaque
             * texture; overlays on top (the grass block's tinted fringe,
             * cut out) do not make it see-through. */
            int covered = 0;
            if (s->shape->full_cube)
                for (int f = 0; f < FACE_COUNT; f++)
                    for (uint32_t i = 0; i < s->shape->element_count; i++) {
                        const blockmodel_element_t *e = &s->shape->elements[i];
                        const canvas_t *t = e->faces[f].texture;
                        int whole = e->rot_angle == 0 && !e->has_xf &&
                            (f < 2 ? e->x0 <= 0 && e->x1 >= 16 && e->z0 <= 0 && e->z1 >= 16 && (f == FACE_UP ? e->y1 >= 16 : e->y0 <= 0)
                             : f < 4 ? e->x0 <= 0 && e->x1 >= 16 && e->y0 <= 0 && e->y1 >= 16 && (f == FACE_NORTH ? e->z0 <= 0 : e->z1 >= 16)
                             : e->y0 <= 0 && e->y1 >= 16 && e->z0 <= 0 && e->z1 >= 16 && (f == FACE_EAST ? e->x1 >= 16 : e->x0 <= 0));
                        if (t && whole && tex_opaque(t)) { covered++; break; }
                    }
            if (covered == FACE_COUNT) opaque = 1;
            s->self_cull = !opaque && !name_has(bare, "leaves");
            s->occ_y1 = 16;
            if (s->shape->full_cube) {
                s->cull = opaque;
                s->occl = opaque || name_has(bare, "leaves");
                s->leaves = (uint8_t)name_has(bare, "leaves");
            } else {
                s->occl = shape_volume(s->shape) >= 0.5;
                /* a box over the whole floor of the voxel blocks only at its
                 * own heights: as a whole voxel, a slab floor's neighbours
                 * hid half the sky from each other, a step up from the ground */
                float bx0 = 16, bx1 = 0, by0 = 16, by1 = 0, bz0 = 16, bz1 = 0;
                for (uint32_t i = 0; i < s->shape->element_count; i++) {
                    const blockmodel_element_t *e = &s->shape->elements[i];
                    bx0 = fminf(bx0, e->x0); bx1 = fmaxf(bx1, e->x1);
                    by0 = fminf(by0, e->y0); by1 = fmaxf(by1, e->y1);
                    bz0 = fminf(bz0, e->z0); bz1 = fmaxf(bz1, e->z1);
                }
                if (s->occl && bx0 <= 0 && bz0 <= 0 && bx1 >= 16 && bz1 >= 16) {
                    s->occ_y0 = (uint8_t)(by0 < 0 ? 0 : by0);
                    s->occ_y1 = (uint8_t)(by1 > 16 ? 16 : by1);
                }
                /* plants are cut-out planes (crosses, vines); a cut-out box
                 * (a head, a lantern, a banner) is lit per face, and so are
                 * planes that are not plants (a ladder, a rail, dripstone) */
                int flat = 1;
                for (uint32_t i = 0; i < s->shape->element_count; i++) {
                    const blockmodel_element_t *e = &s->shape->elements[i];
                    if (e->x1 > e->x0 && e->y1 > e->y0 && e->z1 > e->z0) flat = 0;
                }
                s->up_lit = !opaque && !s->occl && flat && is_plant(bare);
                int clear = 0; /* see-through: glass panes */
                for (uint32_t i = 0; i < s->shape->element_count; i++)
                    for (int f = 0; f < FACE_COUNT; f++) {
                        const canvas_t *t = s->shape->elements[i].faces[f].texture;
                        if (!t) continue;
                        uint32_t k = tex_slot(t);
                        if (slots[k].has_partial) clear = 1;
                    }
                s->far_occl = !s->occl && !s->up_lit && !flat && !clear && s->shape->element_count;
            }
        }
        /* the offset type lives in the game's code, not in the jar's data */
        s->jitter = name_is(bare, "short_grass") || name_is(bare, "fern") ||
                    name_is(bare, "short_dry_grass") || name_is(bare, "tall_dry_grass");
        s->tint_kind = (uint8_t)tint_classify(name, nlen, s->tint_fixed);
        size_t bl = strlen(bare);
        s->be_tex = (bl > 7 && strcmp(bare + bl - 7, "_banner") == 0) ||
                    name_is(bare, "player_head") || name_is(bare, "player_wall_head") || name_is(bare, "decorated_pot");
        int e = emission(bare, key);
        s->emit = (uint8_t)(e < 0 ? 0 : e > 15 ? 15 : e);
    }
    htab[h] = ++nstates;
    return nstates - 1;
}
